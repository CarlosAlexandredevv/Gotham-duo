"""Virtual Xbox 360 Controller (Player 2) driven by Numpad or IJKL + F7/F8 keys."""

import ctypes
import sys
import time

try:
    import vgamepad as vg
except Exception as exc:
    print(f"Erro ao inicializar 'vgamepad' (driver ViGEmBus instalado?): {exc}")
    sys.exit(1)

GetAsyncKeyState = ctypes.windll.user32.GetAsyncKeyState
GetAsyncKeyState.argtypes = [ctypes.c_int]
GetAsyncKeyState.restype = ctypes.c_short

VK_F7 = 0x76
VK_F8 = 0x77
VK_F12 = 0x7B

VK_NUMPAD0 = 0x60
VK_NUMPAD1 = 0x61
VK_NUMPAD2 = 0x62
VK_NUMPAD3 = 0x63
VK_NUMPAD4 = 0x64
VK_NUMPAD5 = 0x65
VK_NUMPAD6 = 0x66
VK_NUMPAD7 = 0x67
VK_NUMPAD8 = 0x68
VK_NUMPAD9 = 0x69
VK_ADD = 0x6B
VK_SUBTRACT = 0x6D

# Letter fallbacks (work regardless of NumLock or compact keyboards)
VK_I, VK_J, VK_K, VK_L = 0x49, 0x4A, 0x4B, 0x4C
VK_U, VK_O, VK_M, VK_COMMA = 0x55, 0x4F, 0x4D, 0xBC

BUTTON_MAP = [
    ((VK_NUMPAD1, VK_U), vg.XUSB_BUTTON.XUSB_GAMEPAD_A, "Num1 / U -> A (Pular/Confirmar)"),
    ((VK_NUMPAD2, VK_O), vg.XUSB_BUTTON.XUSB_GAMEPAD_B, "Num2 / O -> B (Especial/Voltar)"),
    ((VK_NUMPAD3, VK_M), vg.XUSB_BUTTON.XUSB_GAMEPAD_X, "Num3 / M -> X (Atacar)"),
    ((VK_NUMPAD0, VK_COMMA), vg.XUSB_BUTTON.XUSB_GAMEPAD_Y, "Num0 / , -> Y (Trocar/Usar)"),
    ((VK_NUMPAD7,), vg.XUSB_BUTTON.XUSB_GAMEPAD_LEFT_SHOULDER, "Num7 -> LB"),
    ((VK_NUMPAD9,), vg.XUSB_BUTTON.XUSB_GAMEPAD_RIGHT_SHOULDER, "Num9 -> RB"),
    ((VK_F8,), vg.XUSB_BUTTON.XUSB_GAMEPAD_START, "F8 -> Start (Entrar Co-op / Duo)"),
    ((VK_F7,), vg.XUSB_BUTTON.XUSB_GAMEPAD_BACK, "F7 -> Back  (F7+F8 800ms -> Solo)"),
]


def any_down(*vks: int) -> bool:
    return any((GetAsyncKeyState(vk) & 0x8000) != 0 for vk in vks)


def main() -> int:
    try:
        pad = vg.VX360Gamepad()
    except Exception as exc:
        print(f"Falha ao criar controle virtual: {exc}")
        return 1

    print("Controle virtual Xbox 360 (P2) conectado!")
    print("  Num8/5/4/6 ou I/K/J/L -> Analogico esquerdo")
    print("  Num+ / Num-           -> RT / LT")
    for _, _, label in BUTTON_MAP:
        print(f"  {label}")
    print("  F12 ou Ctrl+C -> Sair e desconectar")

    last_state = None
    try:
        while not any_down(VK_F12):
            x = float(any_down(VK_NUMPAD6, VK_L)) - float(any_down(VK_NUMPAD4, VK_J))
            y = float(any_down(VK_NUMPAD8, VK_I)) - float(any_down(VK_NUMPAD5, VK_K))
            pad.left_joystick_float(x_value_float=x, y_value_float=y)

            rt = 1.0 if any_down(VK_ADD) else 0.0
            lt = 1.0 if any_down(VK_SUBTRACT) else 0.0
            pad.right_trigger_float(rt)
            pad.left_trigger_float(lt)

            pressed_names = []
            for vks, btn, label in BUTTON_MAP:
                if any_down(*vks):
                    pad.press_button(button=btn)
                    pressed_names.append(label.split("->")[1].strip().split()[0])
                else:
                    pad.release_button(button=btn)

            pad.update()

            cur_state = (x, y, rt, lt, tuple(pressed_names))
            if cur_state != last_state:
                last_state = cur_state
                if x != 0.0 or y != 0.0 or rt != 0.0 or lt != 0.0 or pressed_names:
                    print(f"[P2] stick=({x:+.0f},{y:+.0f}) btn={pressed_names}")

            time.sleep(0.01)
    except KeyboardInterrupt:
        pass
    finally:
        pad.reset()
        pad.update()
        print("Controle virtual desconectado.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
