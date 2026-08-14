#include <cstdio>
#include <cstring>
#include <algorithm>

#include "pico/time.h"
#include "tusb.h"
#include "USBDevice/DeviceDriver/XInput/tud_xinput/tud_xinput.h"
#include "USBDevice/DeviceDriver/XInput/XInput.h"

extern "C" {
#include "xsm3.h"
}

namespace {
	enum class Xsm3AuthState : uint8_t {
		Idle = 0,
		Responded = 1,      // init response ready for 0x83
		Authenticated = 2,  // verify response ready for 0x83
	};
	static Xsm3AuthState xsm3_auth_state = Xsm3AuthState::Idle;
	static uint8_t xsm3_buf_82[0x40];
	static uint8_t xsm3_buf_87[0x40];
	static uint8_t xsm3_state_buf[2] = { 0x01, 0x00 };

	static constexpr uint8_t XSM3_STATE_PROCESSING = 1;
	static constexpr uint8_t XSM3_STATE_READY = 2;
	static constexpr uint16_t XSM3_RESPONSE_INIT_LEN = 46u;
	static constexpr uint16_t XSM3_RESPONSE_VERIFY_LEN = 0x16u;
}

void XInputDevice::initialize()
{
	class_driver_ = *tud_xinput::class_driver();
	xsm3_initialise_state();
	xsm3_set_identification_data(xsm3_id_data_ms_controller);
	xsm3_auth_state = Xsm3AuthState::Idle;
}

void XInputDevice::process(const uint8_t idx, Gamepad& gamepad)
{
	in_report_.buttons[0] = 0;
	in_report_.buttons[1] = 0;
	Gamepad::PadIn gp_in = gamepad.get_pad_in();

	switch (gp_in.dpad)
	{
		case Gamepad::DPAD_UP:
			in_report_.buttons[0] = XInput::Buttons0::DPAD_UP;
			break;
		case Gamepad::DPAD_DOWN:
			in_report_.buttons[0] = XInput::Buttons0::DPAD_DOWN;
			break;
		case Gamepad::DPAD_LEFT:
			in_report_.buttons[0] = XInput::Buttons0::DPAD_LEFT;
			break;
		case Gamepad::DPAD_RIGHT:
			in_report_.buttons[0] = XInput::Buttons0::DPAD_RIGHT;
			break;
		case Gamepad::DPAD_UP_LEFT:
			in_report_.buttons[0] = XInput::Buttons0::DPAD_UP | XInput::Buttons0::DPAD_LEFT;
			break;
		case Gamepad::DPAD_UP_RIGHT:
			in_report_.buttons[0] = XInput::Buttons0::DPAD_UP | XInput::Buttons0::DPAD_RIGHT;
			break;
		case Gamepad::DPAD_DOWN_LEFT:
			in_report_.buttons[0] = XInput::Buttons0::DPAD_DOWN | XInput::Buttons0::DPAD_LEFT;
			break;
		case Gamepad::DPAD_DOWN_RIGHT:
			in_report_.buttons[0] = XInput::Buttons0::DPAD_DOWN | XInput::Buttons0::DPAD_RIGHT;
			break;
		default:
			break;
	}

	if (gp_in.buttons & Gamepad::BUTTON_BACK)  in_report_.buttons[0] |= XInput::Buttons0::BACK;
	if (gp_in.buttons & Gamepad::BUTTON_START) in_report_.buttons[0] |= XInput::Buttons0::START;
	if (gp_in.buttons & Gamepad::BUTTON_L3)    in_report_.buttons[0] |= XInput::Buttons0::L3;
	if (gp_in.buttons & Gamepad::BUTTON_R3)    in_report_.buttons[0] |= XInput::Buttons0::R3;

	if (gp_in.buttons & Gamepad::BUTTON_X)     in_report_.buttons[1] |= XInput::Buttons1::X;
	if (gp_in.buttons & Gamepad::BUTTON_A)     in_report_.buttons[1] |= XInput::Buttons1::A;
	if (gp_in.buttons & Gamepad::BUTTON_Y)     in_report_.buttons[1] |= XInput::Buttons1::Y;
	if (gp_in.buttons & Gamepad::BUTTON_B)     in_report_.buttons[1] |= XInput::Buttons1::B;
	if (gp_in.buttons & Gamepad::BUTTON_LB)    in_report_.buttons[1] |= XInput::Buttons1::LB;
	if (gp_in.buttons & Gamepad::BUTTON_RB)    in_report_.buttons[1] |= XInput::Buttons1::RB;
	if (gp_in.buttons & Gamepad::BUTTON_SYS)   in_report_.buttons[1] |= XInput::Buttons1::HOME;

	in_report_.trigger_l = gp_in.trigger_l;
	in_report_.trigger_r = gp_in.trigger_r;

	in_report_.joystick_lx = gp_in.joystick_lx;
	in_report_.joystick_ly = Range::invert(gp_in.joystick_ly);
	in_report_.joystick_rx = gp_in.joystick_rx;
	in_report_.joystick_ry = Range::invert(gp_in.joystick_ry);

	// Remote wake
	{
		static bool start_wake_sent = false;
		static bool start_held = false;
		static absolute_time_t start_hold_begin = { 0 };
		bool start_pressed = (gp_in.buttons & Gamepad::BUTTON_START) != 0;
		if (start_pressed)
		{
			if (!start_held)
			{
				start_held = true;
				start_hold_begin = get_absolute_time();
			}
			else
			{
				uint64_t hold_ms = to_ms_since_boot(get_absolute_time()) - to_ms_since_boot(start_hold_begin);
				if (hold_ms >= 3000 && tud_suspended() && !start_wake_sent)
				{
					tud_remote_wakeup();
					start_wake_sent = true;
				}
			}
		}
		else
		{
			start_held = false;
			start_wake_sent = false;
		}
		if (tud_suspended() && (gp_in.buttons & Gamepad::BUTTON_SYS))
			tud_remote_wakeup();
	}

	tud_xinput::send_report((uint8_t*)&in_report_, sizeof(XInput::InReport));

	if (tud_xinput::receive_report(reinterpret_cast<uint8_t*>(&out_report_), sizeof(XInput::OutReport)) &&
		out_report_.report_id == XInput::OutReportID::RUMBLE)
	{
		Gamepad::PadOut gp_out;
		gp_out.rumble_l = out_report_.rumble_l;
		gp_out.rumble_r = out_report_.rumble_r;
		gamepad.set_pad_out(gp_out);
	}
}

uint16_t XInputDevice::get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) 
{
	std::memcpy(buffer, &in_report_, sizeof(XInput::InReport));
	return sizeof(XInput::InReport);
}

void XInputDevice::set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {}

bool XInputDevice::vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
	uint8_t type_bits = request->bmRequestType & 0x60;
	if (type_bits != 0x40 && type_bits != 0x20)
		return false;

	switch (request->bRequest)
	{
	// 0x81: Host GET — send controller identification (0x1D bytes).
	case 0x81:
		if (stage == CONTROL_STAGE_SETUP)
		{
			uint16_t len = std::min(request->wLength, (uint16_t)0x1D);
			return tud_control_xfer(rhport, request, const_cast<uint8_t*>(xsm3_id_data_ms_controller), len);
		}
		return true;

	// 0x82: Host OUT — receive challenge init
	case 0x82:
		if (stage == CONTROL_STAGE_SETUP)
		{
			uint16_t len = std::min(request->wLength, (uint16_t)sizeof(xsm3_buf_82));
			return tud_control_xfer(rhport, request, xsm3_buf_82, len);
		}
		else if (stage == CONTROL_STAGE_DATA || stage == CONTROL_STAGE_ACK)
		{
			xsm3_do_challenge_init(xsm3_buf_82);
			xsm3_auth_state = Xsm3AuthState::Responded;
		}
		return true;

	// 0x83: Host GET — send challenge response
	case 0x83:
		if (stage == CONTROL_STAGE_SETUP)
		{
			if (xsm3_auth_state == Xsm3AuthState::Authenticated)
			{
				uint16_t len = std::min(request->wLength, XSM3_RESPONSE_VERIFY_LEN);
				return tud_control_xfer(rhport, request, xsm3_challenge_response, len);
			}
			else
			{
				uint16_t len = std::min(request->wLength, XSM3_RESPONSE_INIT_LEN);
				return tud_control_xfer(rhport, request, xsm3_challenge_response, len);
			}
		}
		return true;

	// 0x84: Host GET — keepalive, zero-length response
	case 0x84:
		if (stage == CONTROL_STAGE_SETUP)
		{
			return tud_control_xfer(rhport, request, nullptr, 0);
		}
		return true;

	// 0x86: Host GET — state (1 = processing, 2 = ready)
	case 0x86:
		if (stage == CONTROL_STAGE_SETUP)
		{
			uint8_t state_val = (xsm3_auth_state == Xsm3AuthState::Responded || xsm3_auth_state == Xsm3AuthState::Authenticated)
				? XSM3_STATE_READY : XSM3_STATE_PROCESSING;
			xsm3_state_buf[0] = state_val;
			xsm3_state_buf[1] = 0x00;
			uint16_t len = std::min(request->wLength, (uint16_t)2);
			return tud_control_xfer(rhport, request, xsm3_state_buf, len);
		}
		return true;

	// 0x87: Host OUT — receive verify
	case 0x87:
		if (stage == CONTROL_STAGE_SETUP)
		{
			uint16_t len = std::min(request->wLength, (uint16_t)sizeof(xsm3_buf_87));
			return tud_control_xfer(rhport, request, xsm3_buf_87, len);
		}
		else if (stage == CONTROL_STAGE_DATA || stage == CONTROL_STAGE_ACK)
		{
			xsm3_do_challenge_verify(xsm3_buf_87);
			xsm3_auth_state = Xsm3AuthState::Authenticated;
		}
		return true;

	default:
		return false;
	}
}

const uint16_t * XInputDevice::get_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	if (index == 4)
	{
		static uint16_t xsm3_str[96];
		const char *str = reinterpret_cast<const char*>(XInput::STRING_XSM3);
		size_t len = std::strlen(str);
		if (len > 95) len = 95;
		for (size_t i = 0; i < len; i++)
			xsm3_str[1 + i] = static_cast<uint8_t>(str[i]);
		xsm3_str[0] = (0x03u << 8) | static_cast<uint16_t>(2 * len + 2);
		return xsm3_str;
	}
	const char *value = reinterpret_cast<const char*>(XInput::DESC_STRING[index]);
	return get_string_descriptor(value, index);
}

const uint8_t * XInputDevice::get_descriptor_device_cb() 
{
	return XInput::DESC_DEVICE;
}

const uint8_t * XInputDevice::get_hid_descriptor_report_cb(uint8_t itf) 
{
	return nullptr;
}

const uint8_t * XInputDevice::get_descriptor_configuration_cb(uint8_t index)
{
	if (index != 0)
		return nullptr;
	return XInput::DESC_CONFIGURATION;
}

const uint8_t * XInputDevice::get_descriptor_device_qualifier_cb() 
{
	return nullptr;
}
