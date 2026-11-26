# Emular o teclado como um segundo controle (gamepad virtual)

É possível fazer o sistema operacional enxergar o teclado como um controle de verdade. O jeito mais comum no Windows é criar um **gamepad virtual** (por exemplo, um controle Xbox 360) e ligar as teclas a ele.

## Opções (do mais fácil ao mais flexível)

| Opção | Tipo | Observações |
|---|---|---|
| **Steam Input** | Já incluso na Steam | Adicione o jogo como "jogo que não é da Steam", abra as configurações do controlador e escolha um template de teclado para gamepad. |
| **reWASD** | Pago, interface gráfica | Mapeia teclas e mouse para um controle virtual. |
| **Universal Control Remapper (UCR)** | Gratuito, interface gráfica | Alternativa gratuita ao reWASD. |
| **ViGEmBus + script próprio** | Driver + código | Máximo de controle. O driver cria o controle virtual; bibliotecas como `vgamepad` (Python) ou `ViGEm.NET` (C#) enviam os inputs. O projeto ViGEm foi descontinuado em 2023, mas ainda funciona bem. |
| **vJoy** | Driver | Cria um joystick DirectInput genérico. Serve para jogos antigos; alguns jogos modernos só reconhecem XInput (Xbox). |

## Exemplo mínimo em Python

```python
# pip install vgamepad keyboard
import vgamepad as vg
import keyboard

pad = vg.VX360Gamepad()

while True:
    # Analógico esquerdo com WASD
    x = (keyboard.is_pressed('d') - keyboard.is_pressed('a'))
    y = (keyboard.is_pressed('w') - keyboard.is_pressed('s'))
    pad.left_joystick_float(x_value_float=x, y_value_float=y)

    # Botões
    for key, btn in [('j', vg.XUSB_BUTTON.XUSB_GAMEPAD_A),
                     ('k', vg.XUSB_BUTTON.XUSB_GAMEPAD_B)]:
        (pad.press_button if keyboard.is_pressed(key) else pad.release_button)(button=btn)

    pad.update()
    if keyboard.is_pressed('esc'):
        break
```

## Pontos de atenção

- **O teclado continua sendo teclado.** Se o jogo também lê o teclado para o jogador 1, as teclas do "controle" vão interferir nele. Soluções:
  - usar teclas que o jogo não usa;
  - usar o **HidHide** para esconder o dispositivo físico do jogo;
  - para separar de verdade dois teclados, usar o driver **Interception**.
- **O jogo precisa aceitar um segundo controle.** Se ele só reconhece o controle como jogador 2 depois de um primeiro conectado, pode ser preciso ter dois controles virtuais, ou um virtual e um físico.
- **Jogos online com anti-cheat** (EAC, BattlEye) podem bloquear drivers de input virtual. Em co-op local offline isso normalmente não é problema.
- **No Linux**, o equivalente é `uinput`/`evdev` (por exemplo, `python-evdev` cria um gamepad virtual). O Steam Input também funciona.

## Próximos passos

1. Definir o sistema operacional e o jogo-alvo.
2. Escolher a opção: Steam Input para algo rápido, ViGEmBus + script para controle total.
3. Definir o mapeamento de teclas (movimento, botões, gatilhos).
4. Testar se o jogo reconhece o controle virtual como jogador 2.
