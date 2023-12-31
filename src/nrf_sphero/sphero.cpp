#include "sphero.hpp"
#include "ble/scanner.h"
#include "ble/sphero_client.h"
#include "controls/packet_collector.hpp"
#include "controls/processors.hpp"
#include "utils/color.hpp"
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>
#include <zephyr/logging/log.h>
#include <zephyr/timing/timing.h>
// COMMANDS
#include "commands/drive.hpp"
#include "commands/io.hpp"
#include "commands/power.hpp"
#include "commands/sensor.hpp"

LOG_MODULE_REGISTER(Sphero, LOG_LEVEL_DBG);

#include <iomanip>
#include <sstream>

uint8_t Sphero::received_cb_wrapper(struct bt_sphero_client* sphero, const uint8_t* data, uint16_t len, void* context)
{

    Sphero* sphero_instance = static_cast<Sphero*>(context);

    sphero_instance->packet_collector->add_packet(data, len);

    return 1;
}

void Sphero::handle_packet(Packet packet)
{
    auto id = packet.id();

    if (waiting.find(id) == waiting.end()) {
        // NOTE: We don't want to log this since it will happen a lot
        // NOTE: There are packets which we should handle related to disconnecting
        // LOG_ERR("No signal found for packet id %d", id);
        return;
    }

    auto signal = waiting[id];

    responses.insert_or_assign(id, packet);

    k_poll_signal_raise(signal.get(), id);
}

void Sphero::subscribe()
{
    bt_sphero_client* sphero_client = scanner_get_sphero(sphero_id);

    if (sphero_client == nullptr) {
        LOG_ERR("Sphero not found");
        return;
    }

    int err = bt_sphero_subscribe(sphero_client, &received_cb_wrapper, this);
    if (err) {
        LOG_ERR("Failed to subscribe to notifications (err %d)", err);
    }

    scanner_release_sphero(sphero_client);
}

Sphero::Sphero(uint8_t id)
{
    sphero_id = id;
    frame_index = 0;
    animation_index = 0;

    packet_manager = new PacketManager();

    packet_collector = new PacketCollector(std::bind(&Sphero::handle_packet, this, std::placeholders::_1));

    subscribe();

    auto response = wake_with_response();

    wait_for_response(response);

    turn_off_all_leds();
};

Sphero::~Sphero()
{
    delete packet_manager;
};

void Sphero::execute(const Packet& packet, bool test)
{

    auto payload = packet.build();

    bt_sphero_client* sphero_client = scanner_get_sphero(sphero_id);

    if (sphero_client == nullptr) {
        LOG_ERR("Sphero not found");
        return;
    }

    const size_t chunkSize = 20;
    size_t offset = 0;

    while (offset < payload.size()) {
        size_t remainingBytes = payload.size() - offset;
        size_t bytesToSend = chunkSize < remainingBytes ? chunkSize : remainingBytes;

        if (!test) {
            int err = bt_sphero_client_send(sphero_client, payload.data() + offset, bytesToSend);

            if (err) {
                LOG_ERR("Error sending data!");
            }
        }

        offset += bytesToSend;
    }

    scanner_release_sphero(sphero_client);
};

CommandResponse Sphero::setup_response(const Packet& packet)
{
    std::shared_ptr<struct k_poll_signal> signal = std::make_shared<struct k_poll_signal>();

    k_poll_signal_init(signal.get());

    auto events = std::make_unique<k_poll_event[]>(1);
    events[0] = K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, signal.get());

    auto id = packet.id();

    waiting.insert_or_assign(id, signal);

    return events;
}

void Sphero::wake()
{
    auto packet = Power::wake(*this);

    execute(packet);
}

CommandResponse Sphero::wake_with_response()
{
    auto packet = Power::wake(*this);

    execute(packet);

    return setup_response(packet);
}

void Sphero::set_locator_flags(bool locator_flags)
{
    auto packet = Sensor::set_locator_flags(*this, locator_flags, static_cast<uint8_t>(Processors::SECONDARY));

    execute(packet);
}

void Sphero::set_matrix_fill(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2, RGBColor color)
{
    auto packet = IO::fill_led_matrix(*this, x1, y1, x2, y2, color, static_cast<uint8_t>(Processors::SECONDARY));

    execute(packet);
}

void Sphero::set_matrix_color(RGBColor color)
{
    auto packet = IO::set_led_matrix_color(*this, color, static_cast<uint8_t>(Processors::SECONDARY));

    execute(packet);
}

void Sphero::set_matrix_pixel_color(uint8_t x, uint8_t y, RGBColor color)
{
    auto packet = IO::set_led_matrix_pixel_color(*this, x, y, color, static_cast<uint8_t>(Processors::SECONDARY));

    execute(packet);
}

void Sphero::set_matrix_character(unsigned char str, RGBColor color)
{
    auto packet = IO::set_led_matrix_character(*this, str, color, static_cast<uint8_t>(Processors::SECONDARY));

    execute(packet);
}

void Sphero::save_compressed_frame(uint8_t index, std::vector<uint8_t> frame)
{
    auto packet = IO::save_compressed_frame(*this, index, frame, static_cast<uint8_t>(Processors::SECONDARY));

    execute(packet);
}

CommandResponse Sphero::save_compressed_frame_with_response(uint8_t index, std::vector<uint8_t> frame)
{
    auto packet = IO::save_compressed_frame(*this, index, frame, static_cast<uint8_t>(Processors::SECONDARY));

    execute(packet);

    return setup_response(packet);
}

void Sphero::save_compressed_frame_animation(uint8_t fps, bool fade_animation, std::vector<RGBColor> palette, std::vector<uint16_t> frame_indexes)
{
    auto packet = IO::save_compressed_frame_animation(*this, animation_index, fps, fade_animation, palette, frame_indexes, static_cast<uint8_t>(Processors::SECONDARY));

    animation_index++;

    execute(packet);
}

void Sphero::register_matrix_animation(std::vector<std::vector<std::vector<uint8_t>>> frames, std::vector<RGBColor> palette, uint8_t fps, bool transition)
{
    std::vector<uint16_t> frame_indexes = {};

    for (auto frame : frames) {
        std::vector<uint8_t> compressed_frame = {};
        for (uint8_t idx = 0; idx < 4; idx++) {
            for (uint8_t row_idx = 7; row_idx != UINT8_MAX; row_idx--) {
                uint8_t res = 0;
                for (uint8_t col_idx = 0; col_idx < 8; col_idx++) {
                    uint8_t bit = (frame[row_idx][col_idx] & 1 << idx) >> idx;
                    res |= bit << (7 - col_idx);
                }
                compressed_frame.push_back(res);
            }
        }
        save_compressed_frame(frame_index, compressed_frame);
        k_msleep(250);
        // wait_for_response(response);
        frame_indexes.push_back(frame_index);
        frame_index++;
    }

    save_compressed_frame_animation(fps, transition, palette, frame_indexes);
}

void Sphero::play_animation(uint8_t animation_id, bool loop)
{
    auto packet = IO::play_animation(*this, animation_id, loop, static_cast<uint8_t>(Processors::SECONDARY));

    execute(packet);
}

void Sphero::clear_matrix()
{
    auto packet = IO::clear_matrix(*this, static_cast<uint8_t>(Processors::SECONDARY));

    execute(packet);
}

void Sphero::set_all_leds_with_8_bit_mask(uint8_t mask, std::vector<uint8_t> led_values)
{
    auto packet = IO::set_all_leds_with_8_bit_mask(*this, mask, led_values, static_cast<uint8_t>(Processors::PRIMARY));
    execute(packet);
}

void Sphero::set_all_leds_with_map(std::unordered_map<Sphero::LEDs, uint8_t> mapping)
{
    uint8_t mask = 0;

    std::vector<uint8_t> led_values = {};

    for (int i = 0; i < static_cast<int>(Sphero::LEDs::LAST); ++i) {
        Sphero::LEDs led = static_cast<Sphero::LEDs>(i);
        if (mapping.find(led) != mapping.end()) {
            mask |= 1 << static_cast<uint8_t>(led);
            led_values.push_back(mapping[led]);
        }
    }

    if (mask != 0) {
        set_all_leds_with_8_bit_mask(mask, led_values);
    }
}

void Sphero::turn_off_all_leds()
{
    std::unordered_map<Sphero::LEDs, uint8_t> mapping;

    for (int i = 0; i < static_cast<int>(Sphero::LEDs::LAST); ++i) {
        mapping[static_cast<Sphero::LEDs>(i)] = 0;
    }

    set_all_leds_with_map(mapping);
    set_matrix_color(RGBColor(0, 0, 0));
}

Packet Sphero::get_drive_packet(uint8_t speed, uint16_t heading)
{
    DriveFlags flag = DriveFlags::FORWARD;

    // if (speed < 0) {
    //     flag = DriveFlags::BACKWARD;
    //     heading = (heading + 180) % 360;
    // }

    speed = speed > 255 ? 255 : speed;

    auto packet = Drive::drive(*this, speed, heading, flag, static_cast<uint8_t>(Processors::SECONDARY));

    return packet;
}

void Sphero::drive(uint8_t speed, uint16_t heading)
{
    auto packet = get_drive_packet(speed, heading);

    execute(packet);
}

CommandResponse Sphero::drive_with_response(uint8_t speed, uint16_t heading)
{
    auto packet = get_drive_packet(speed, heading);

    execute(packet);

    return setup_response(packet);
}

void Sphero::set_heading(uint16_t heading)
{
    drive(0, heading);
}

void Sphero::reset_aim()
{
    auto packet = Drive::reset_aim(*this, static_cast<uint8_t>(Processors::SECONDARY));

    execute(packet);
}

std::optional<Packet> Sphero::wait_for_response(const CommandResponse& response)
{
    int err = 0;

    err = k_poll(response.get(), 1, K_MSEC(10000));

    if (err) {
        LOG_ERR("Failed to wait for response (err %d)", err);
        return std::nullopt;
    }

    auto res = response.get()->signal->result;

    auto packet = responses.find(res);

    if (packet == responses.end()) {
        LOG_ERR("No packet for packet id %d", res);
        return std::nullopt;
    }

    // Ideally this would be done in the handle_packet but doing it there causes signal to be destroyed so we
    // cannot get back the id
    waiting.erase(res);
    responses.erase(res);

    return packet->second;
}