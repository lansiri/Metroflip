#include <flipper_application.h>

#include <lib/nfc/protocols/mf_desfire/mf_desfire.h>
#include <nfc/protocols/mf_desfire/mf_desfire_poller.h>
#include <stdio.h>

#include "../../metroflip_i.h"
#include "../../api/metroflip/metroflip_api.h"
#include "../../metroflip_plugins.h"

#define TAG "Metroflip:Scene:HSL"

// Application and file identifiers for HSL (v2)
static const MfDesfireApplicationId hsl_app_id = {.data = {0x11, 0x20, 0xEF}};
static const MfDesfireFileId hsl_file_app_info = 0x08;
static const MfDesfireFileId hsl_file_period = 0x01;
static const MfDesfireFileId hsl_file_stored_value = 0x02;

// Helpers --------------------------------------------------------------------
static uint32_t extract_bits_be(const uint8_t* data, size_t bit_offset, size_t bit_length) {
    uint32_t value = 0;
    for(size_t i = 0; i < bit_length; i++) {
        size_t absolute_bit = bit_offset + i;
        size_t byte_index = absolute_bit / 8;
        size_t bit_in_byte = 7 - (absolute_bit % 8);
        value = (value << 1) | ((data[byte_index] >> bit_in_byte) & 0x01);
    }
    return value;
}

static void datestamp_to_datetime(uint32_t days_since_1997, DateTime* out) {
    const uint64_t unix_timestamp = 852076800ULL + ((uint64_t)days_since_1997 * 24 * 60 * 60);
    datetime_timestamp_to_datetime(unix_timestamp, out);
}

static void append_date(FuriString* out, uint32_t days_since_1997) {
    if(days_since_1997 == 0) {
        furi_string_cat(out, "No expiry");
        return;
    }

    DateTime dt = {0};
    datestamp_to_datetime(days_since_1997, &dt);

    FuriString* tmp = furi_string_alloc();
    locale_format_date(tmp, &dt, locale_get_date_format(), "-");
    furi_string_cat(out, tmp);
    furi_string_free(tmp);
}

static void append_time(FuriString* out, uint32_t minutes_since_midnight) {
    furi_string_cat_printf(out, "%02lu:%02lu", minutes_since_midnight / 60, minutes_since_midnight % 60);
}

static void append_card_number(FuriString* out, const uint8_t* bcd, size_t len) {
    for(size_t i = 0; i < len; i++) {
        uint8_t hi = (bcd[i] >> 4) & 0x0F;
        uint8_t lo = bcd[i] & 0x0F;
        furi_string_push_back(out, '0' + hi);
        furi_string_push_back(out, '0' + lo);
        // group nicely (6-6-6)
        if((i % 3 == 2) && (i + 1 < len)) furi_string_push_back(out, ' ');
    }
}

// Parsing --------------------------------------------------------------------
static bool hsl_parse(const MfDesfireData* data, FuriString* parsed_data) {
    furi_assert(parsed_data);

    const MfDesfireApplication* app = mf_desfire_get_application(data, &hsl_app_id);
    if(app == NULL) return false;

    furi_string_set(parsed_data, "\e#HSL (v2)\n");

    // Application Info - card number
    do {
        const MfDesfireFileData* file_data = mf_desfire_get_file_data(app, &hsl_file_app_info);
        if(!file_data) break;
        const uint8_t* bytes = simple_array_cget_data(file_data->data);
        size_t len = simple_array_get_count(file_data->data);
        if(len < 11) break; // need at least 10.5 bytes

        furi_string_cat(parsed_data, "Card: ");
        append_card_number(parsed_data, bytes + 1, 9); // BCD[18] starting from byte 1
        furi_string_push_back(parsed_data, '\n');
    } while(false);

    // Stored value
    do {
        const MfDesfireFileData* file_data = mf_desfire_get_file_data(app, &hsl_file_stored_value);
        if(!file_data) break;
        const uint8_t* bytes = simple_array_cget_data(file_data->data);
        size_t len = simple_array_get_count(file_data->data);
        if(len < 13) break;

        const uint32_t balance_cents = extract_bits_be(bytes, 0, 20);
        const uint32_t load_date = extract_bits_be(bytes, 20, 14);
        const uint32_t load_time = extract_bits_be(bytes, 34, 11);
        const uint32_t loaded_value = extract_bits_be(bytes, 45, 20);

        furi_string_cat_printf(
            parsed_data,
            "Balance: %lu.%02lu €\n",
            (unsigned long)(balance_cents / 100),
            (unsigned long)(balance_cents % 100));

        furi_string_cat(parsed_data, "Last load: ");
        append_date(parsed_data, load_date);
        furi_string_cat(parsed_data, " ");
        append_time(parsed_data, load_time);
        furi_string_cat_printf(
            parsed_data,
            " (+%lu.%02lu €)\n",
            (unsigned long)(loaded_value / 100),
            (unsigned long)(loaded_value % 100));
    } while(false);

    // Period product #1 (most common)
    do {
        const MfDesfireFileData* file_data = mf_desfire_get_file_data(app, &hsl_file_period);
        if(!file_data) break;
        const uint8_t* bytes = simple_array_cget_data(file_data->data);
        size_t len = simple_array_get_count(file_data->data);
        if(len < 35) break;

        const uint32_t product_type = extract_bits_be(bytes, 0, 1);
        const uint32_t product_code = extract_bits_be(bytes, 1, 14);
        const uint32_t area_type = extract_bits_be(bytes, 15, 2);
        const uint32_t area = extract_bits_be(bytes, 17, 6);
        const uint32_t start_date = extract_bits_be(bytes, 23, 14);
        const uint32_t end_date = extract_bits_be(bytes, 37, 14);

        furi_string_cat(parsed_data, "Period 1:\n");
        furi_string_cat_printf(
            parsed_data,
            "  Product: %s %lu (area type %lu, area %lu)\n",
            product_type ? "2014" : "2010",
            (unsigned long)product_code,
            (unsigned long)area_type,
            (unsigned long)area);
        furi_string_cat(parsed_data, "  Start: ");
        append_date(parsed_data, start_date);
        furi_string_push_back(parsed_data, '\n');
        furi_string_cat(parsed_data, "  End: ");
        append_date(parsed_data, end_date);
        furi_string_push_back(parsed_data, '\n');
    } while(false);

    return true;
}

// Poller & plugin lifecycle --------------------------------------------------
static NfcCommand hsl_poller_callback(NfcGenericEvent event, void* context) {
    furi_assert(event.protocol == NfcProtocolMfDesfire);

    Metroflip* app = context;
    NfcCommand command = NfcCommandContinue;

    FuriString* parsed_data = furi_string_alloc();
    Widget* widget = app->widget;
    furi_string_reset(app->text_box_store);
    const MfDesfirePollerEvent* mf_desfire_event = event.event_data;
    if(mf_desfire_event->type == MfDesfirePollerEventTypeReadSuccess) {
        nfc_device_set_data(
            app->nfc_device, NfcProtocolMfDesfire, nfc_poller_get_data(app->poller));
        const MfDesfireData* data = nfc_device_get_data(app->nfc_device, NfcProtocolMfDesfire);
        if(!hsl_parse(data, parsed_data)) {
            furi_string_reset(app->text_box_store);
            FURI_LOG_I(TAG, "Unknown card type");
            furi_string_printf(parsed_data, "\e#Unknown card\n");
        }
        widget_add_text_scroll_element(widget, 0, 0, 128, 64, furi_string_get_cstr(parsed_data));

        widget_add_button_element(
            widget, GuiButtonTypeRight, "Exit", metroflip_exit_widget_callback, app);
        widget_add_button_element(
            widget, GuiButtonTypeCenter, "Save", metroflip_save_widget_callback, app);

        furi_string_free(parsed_data);
        view_dispatcher_switch_to_view(app->view_dispatcher, MetroflipViewWidget);
        metroflip_app_blink_stop(app);
        command = NfcCommandStop;
    } else if(mf_desfire_event->type == MfDesfirePollerEventTypeReadFailed) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MetroflipCustomEventPollerSuccess);
        command = NfcCommandContinue;
    }

    return command;
}

static void hsl_on_enter(Metroflip* app) {
    dolphin_deed(DolphinDeedNfcRead);

    if(app->data_loaded) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        FlipperFormat* ff = flipper_format_file_alloc(storage);
        if(flipper_format_file_open_existing(ff, app->file_path)) {
            MfDesfireData* data = mf_desfire_alloc();
            mf_desfire_load(data, ff, 2);
            FuriString* parsed_data = furi_string_alloc();
            Widget* widget = app->widget;

            furi_string_reset(app->text_box_store);
            if(!hsl_parse(data, parsed_data)) {
                furi_string_reset(app->text_box_store);
                FURI_LOG_I(TAG, "Unknown card type");
                furi_string_printf(parsed_data, "\e#Unknown card\n");
            }
            widget_add_text_scroll_element(
                widget, 0, 0, 128, 64, furi_string_get_cstr(parsed_data));

            widget_add_button_element(
                widget, GuiButtonTypeRight, "Exit", metroflip_exit_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeCenter, "Delete", metroflip_delete_widget_callback, app);
            mf_desfire_free(data);
            furi_string_free(parsed_data);
            view_dispatcher_switch_to_view(app->view_dispatcher, MetroflipViewWidget);
        }
        flipper_format_free(ff);
    } else {
        Popup* popup = app->popup;
        popup_set_header(popup, "Apply\n card to\nthe back", 68, 30, AlignLeft, AlignTop);
        popup_set_icon(popup, 0, 3, &I_RFIDDolphinReceive_97x61);

        view_dispatcher_switch_to_view(app->view_dispatcher, MetroflipViewPopup);
        nfc_scanner_alloc(app->nfc);
        app->poller = nfc_poller_alloc(app->nfc, NfcProtocolMfDesfire);
        nfc_poller_start(app->poller, hsl_poller_callback, app);

        metroflip_app_blink_start(app);
    }
}

static bool hsl_on_event(Metroflip* app, SceneManagerEvent event) {
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == MetroflipCustomEventCardDetected) {
            Popup* popup = app->popup;
            popup_set_header(popup, "DON'T\nMOVE", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        } else if(event.event == MetroflipCustomEventCardLost) {
            Popup* popup = app->popup;
            popup_set_header(popup, "Card \n lost", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        } else if(event.event == MetroflipCustomEventWrongCard) {
            Popup* popup = app->popup;
            popup_set_header(popup, "WRONG \n CARD", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        } else if(event.event == MetroflipCustomEventPollerFail) {
            Popup* popup = app->popup;
            popup_set_header(popup, "Failed", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, MetroflipSceneStart);
        consumed = true;
    }

    return consumed;
}

static void hsl_on_exit(Metroflip* app) {
    widget_reset(app->widget);
    metroflip_app_blink_stop(app);
    if(app->poller && !app->data_loaded) {
        nfc_poller_stop(app->poller);
        nfc_poller_free(app->poller);
    }
}

/* Actual implementation of app<>plugin interface */
static const MetroflipPlugin hsl_plugin = {
    .card_name = "HSL",
    .plugin_on_enter = hsl_on_enter,
    .plugin_on_event = hsl_on_event,
    .plugin_on_exit = hsl_on_exit,

};

/* Plugin descriptor to comply with basic plugin specification */
static const FlipperAppPluginDescriptor hsl_plugin_descriptor = {
    .appid = METROFLIP_SUPPORTED_CARD_PLUGIN_APP_ID,
    .ep_api_version = METROFLIP_SUPPORTED_CARD_PLUGIN_API_VERSION,
    .entry_point = &hsl_plugin,
};

/* Plugin entry point - must return a pointer to const descriptor  */
const FlipperAppPluginDescriptor* hsl_plugin_ep(void) {
    return &hsl_plugin_descriptor;
}
