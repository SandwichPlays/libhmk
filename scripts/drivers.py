# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <https://www.gnu.org/licenses/>.


from dataclasses import dataclass, field
import re
from typing import Callable


@dataclass
class PlatformIO:
    # PlatformIO board identifier
    board: str
    # Linker script used for the board
    ldscript: str
    # PlatformIO framework identifier
    framework: str
    # PlatformIO platform identifier
    platform: str


@dataclass
class TinyUSB:
    # TinyUSB MCU identifier
    mcu: str


@dataclass
class Bootloader:
    # Flash address of the bootloader
    address: int
    # Magic value to enter bootloader on firmware startup
    magic: int


@dataclass
class NonUniformSectors:
    # Size of each flash sector
    sizes: list[int]


@dataclass
class UniformSectors:
    # Common sector size
    size: int
    # Number of flash sectors
    num_sectors: int


@dataclass
class Flash:
    sector_sizes: NonUniformSectors | UniformSectors
    # Value of an empty word in the flash
    empty_value: int

    # Get the number of flash sectors
    def get_num_sectors(self):
        match self.sector_sizes:
            case NonUniformSectors(sizes):
                return len(sizes)
            case UniformSectors(_, num_sectors):
                return num_sectors

    # Get the total size of the flash
    def get_flash_size(self):
        match self.sector_sizes:
            case NonUniformSectors(sizes):
                return sum(sizes)
            case UniformSectors(size, num_sectors):
                return size * num_sectors

    # Get the size of a flash sector
    def get_sector_size(self, sector: int):
        match self.sector_sizes:
            case NonUniformSectors(sizes):
                assert sector < len(sizes)
                return sizes[sector]
            case UniformSectors(size, num_sectors):
                assert sector < num_sectors
                return size

    # Round up the required size to the minimum number of flash sectors (from the end)
    def round_up_to_flash_sectors(self, required_size: int):
        assert required_size <= self.get_flash_size()
        total = 0
        for i in range(self.get_num_sectors() - 1, -1, -1):
            total += self.get_sector_size(i)
            if total >= required_size:
                return total
        return total


@dataclass
class ADC:
    # Maximum ADC resolution supported by the MCU
    max_resolution: int
    # ADC input pin names, in the same order as the ADC channels
    input_pins: list[str]
    # Convert a list of GPIO pin names to a tuple of (GPIO port names, GPIO pin numbers)
    to_gpio_array: Callable[[list[str]], tuple[list[str], list[str]]]
    # Supported hardware oversampling ratios: ratio -> (ratio_macro, shift_macro) or None for 1x
    oversample_map: dict[int, tuple[str, str] | None] = field(
        default_factory=lambda: {1: None}
    )
    # Supported ADC sample cycle values: cycles -> SDK macro
    sample_cycles_map: dict[float, str] = field(default_factory=dict)

    # Convert a list of ADC input channels or GPIO pin names to a list of ADC input channels
    def to_adc_inputs(self, channels_or_pins: list[str | int]):
        channels: list[int] = []
        for x in channels_or_pins:
            match x:
                case str():
                    if x not in self.input_pins:
                        raise ValueError(
                            f"Pin '{x}' is not a valid ADC input pin. Valid ADC pins: {self.input_pins}"
                        )
                    channels.append(self.input_pins.index(x))
                case int():
                    if x < 0 or x >= len(self.input_pins):
                        raise ValueError(
                            f"ADC channel {x} out of range (0..{len(self.input_pins) - 1})"
                        )
                    channels.append(x)
        return channels

    # Convert a single GPIO pin name to a tuple of (GPIO port name, GPIO pin number)
    def to_gpio(self, pin: str):
        ports, pin_nums = self.to_gpio_array([pin])
        return ports[0], pin_nums[0]


@dataclass
class Metadata:
    bootloader: Bootloader
    flash: Flash
    adc: ADC
    valid_ports: list[str] = field(default_factory=lambda: ["A", "B", "C", "D", "F"])
    # Maps (pin_ch1, pin_ch2) -> (timer_peripheral, mux_ch1, mux_ch2)
    encoder_timers: dict[tuple[str, str], tuple[str, str, str]] = field(
        default_factory=dict
    )
    mux_timer: str | None = None

    def is_valid_gpio_pin(self, pin: str) -> bool:
        m = re.match(r"^([A-Z])(\d+)$", pin)
        if not m:
            return False
        port, num = m.group(1), int(m.group(2))
        return port in self.valid_ports and 0 <= num <= 15

    def resolve_encoder_timer(
        self, pin_a: str, pin_b: str, requested_timer: str | None = None
    ) -> tuple[str, str, str]:
        if requested_timer and requested_timer.upper() == "GPIO":
            return ("NULL", "0", "0")
        for pair in ((pin_a, pin_b), (pin_b, pin_a)):
            if pair in self.encoder_timers:
                tmr, mux_1, mux_2 = self.encoder_timers[pair]
                if requested_timer is None or requested_timer == tmr:
                    if pair == (pin_a, pin_b):
                        return (tmr, mux_1, mux_2)
                    else:
                        return (tmr, mux_2, mux_1)
        # Fallback to Polled GPIO Quadrature on any arbitrary GPIO pins
        return ("NULL", "0", "0")


@dataclass
class Driver:
    tinyusb: TinyUSB
    metadata: Metadata
    platformio: PlatformIO | None = None  # None for CMake-only targets (e.g. HPM)
    hardware_dir: str | None = None


_AT32F405_OVERSAMPLE_MAP: dict[int, tuple[str, str] | None] = {
    1: None,
    2: ("ADC_OVERSAMPLE_RATIO_2", "ADC_OVERSAMPLE_SHIFT_1"),
    4: ("ADC_OVERSAMPLE_RATIO_4", "ADC_OVERSAMPLE_SHIFT_2"),
    8: ("ADC_OVERSAMPLE_RATIO_8", "ADC_OVERSAMPLE_SHIFT_3"),
    16: ("ADC_OVERSAMPLE_RATIO_16", "ADC_OVERSAMPLE_SHIFT_4"),
    32: ("ADC_OVERSAMPLE_RATIO_32", "ADC_OVERSAMPLE_SHIFT_5"),
    64: ("ADC_OVERSAMPLE_RATIO_64", "ADC_OVERSAMPLE_SHIFT_6"),
    128: ("ADC_OVERSAMPLE_RATIO_128", "ADC_OVERSAMPLE_SHIFT_7"),
    256: ("ADC_OVERSAMPLE_RATIO_256", "ADC_OVERSAMPLE_SHIFT_8"),
}

_AT32F405_SAMPLE_CYCLES_MAP: dict[float, str] = {
    1.5: "ADC_SAMPLETIME_1_5",
    7.5: "ADC_SAMPLETIME_7_5",
    13.5: "ADC_SAMPLETIME_13_5",
    28.5: "ADC_SAMPLETIME_28_5",
    41.5: "ADC_SAMPLETIME_41_5",
    55.5: "ADC_SAMPLETIME_55_5",
    71.5: "ADC_SAMPLETIME_71_5",
    239.5: "ADC_SAMPLETIME_239_5",
}

_AT32F405_ENCODER_TIMERS: dict[tuple[str, str], tuple[str, str, str]] = {
    ("A8", "A9"): ("TMR1", "GPIO_MUX_1", "GPIO_MUX_1"),
    ("A0", "A1"): ("TMR2", "GPIO_MUX_1", "GPIO_MUX_1"),
    ("A5", "A1"): ("TMR2", "GPIO_MUX_1", "GPIO_MUX_1"),
    ("A15", "B3"): ("TMR2", "GPIO_MUX_1", "GPIO_MUX_1"),
    ("A5", "B3"): ("TMR2", "GPIO_MUX_1", "GPIO_MUX_1"),
    ("A0", "B3"): ("TMR2", "GPIO_MUX_1", "GPIO_MUX_1"),
    ("A15", "A1"): ("TMR2", "GPIO_MUX_1", "GPIO_MUX_1"),
    ("A6", "A7"): ("TMR3", "GPIO_MUX_2", "GPIO_MUX_2"),
    ("B4", "B5"): ("TMR3", "GPIO_MUX_2", "GPIO_MUX_2"),
    ("C6", "C7"): ("TMR3", "GPIO_MUX_2", "GPIO_MUX_2"),
    ("B6", "B7"): ("TMR4", "GPIO_MUX_2", "GPIO_MUX_2"),
}

_STM32F446_SAMPLE_CYCLES_MAP: dict[float, str] = {
    3.0: "ADC_SAMPLETIME_3CYCLES",
    15.0: "ADC_SAMPLETIME_15CYCLES",
    28.0: "ADC_SAMPLETIME_28CYCLES",
    56.0: "ADC_SAMPLETIME_56CYCLES",
    84.0: "ADC_SAMPLETIME_84CYCLES",
    112.0: "ADC_SAMPLETIME_112CYCLES",
    144.0: "ADC_SAMPLETIME_144CYCLES",
    480.0: "ADC_SAMPLETIME_480CYCLES",
}

_STM32G431_OVERSAMPLE_MAP: dict[int, tuple[str, str] | None] = {
    1: None,
    2: ("ADC_OVERSAMPLING_RATIO_2", "ADC_RIGHTBITSHIFT_1"),
    4: ("ADC_OVERSAMPLING_RATIO_4", "ADC_RIGHTBITSHIFT_2"),
    8: ("ADC_OVERSAMPLING_RATIO_8", "ADC_RIGHTBITSHIFT_3"),
    16: ("ADC_OVERSAMPLING_RATIO_16", "ADC_RIGHTBITSHIFT_4"),
    32: ("ADC_OVERSAMPLING_RATIO_32", "ADC_RIGHTBITSHIFT_5"),
    64: ("ADC_OVERSAMPLING_RATIO_64", "ADC_RIGHTBITSHIFT_6"),
    128: ("ADC_OVERSAMPLING_RATIO_128", "ADC_RIGHTBITSHIFT_7"),
    256: ("ADC_OVERSAMPLING_RATIO_256", "ADC_RIGHTBITSHIFT_8"),
}

_STM32G431_SAMPLE_CYCLES_MAP: dict[float, str] = {
    2.5: "ADC_SAMPLETIME_2CYCLES_5",
    6.5: "ADC_SAMPLETIME_6CYCLES_5",
    12.5: "ADC_SAMPLETIME_12CYCLES_5",
    24.5: "ADC_SAMPLETIME_24CYCLES_5",
    47.5: "ADC_SAMPLETIME_47CYCLES_5",
    92.5: "ADC_SAMPLETIME_92CYCLES_5",
    247.5: "ADC_SAMPLETIME_247CYCLES_5",
    640.5: "ADC_SAMPLETIME_640CYCLES_5",
}


STM32F446XX = Driver(
    platformio=PlatformIO(
        board="genericSTM32F446RE",
        ldscript="stm32f446retx.ld",
        framework="stm32cube",
        platform="ststm32",
    ),
    tinyusb=TinyUSB(mcu="stm32f4"),
    metadata=Metadata(
        bootloader=Bootloader(
            address=0x1FFF0000,
            magic=0xDEADBEEF,
        ),
        flash=Flash(
            sector_sizes=NonUniformSectors(
                sizes=[
                    16 * 1024,
                    16 * 1024,
                    16 * 1024,
                    16 * 1024,
                    64 * 1024,
                    128 * 1024,
                    128 * 1024,
                    128 * 1024,
                ]
            ),
            empty_value=0xFFFFFFFF,
        ),
        adc=ADC(
            max_resolution=12,
            input_pins=[
                "A0",
                "A1",
                "A2",
                "A3",
                "A4",
                "A5",
                "A6",
                "A7",
                "B0",
                "B1",
                "C0",
                "C1",
                "C2",
                "C3",
                "C4",
                "C5",
            ],
            to_gpio_array=lambda pins: (
                [f"GPIO{pin[0]}" for pin in pins],
                [f"GPIO_PIN_{pin[1:]}" for pin in pins],
            ),
            oversample_map={1: None},
            sample_cycles_map=_STM32F446_SAMPLE_CYCLES_MAP,
        ),
        valid_ports=["A", "B", "C", "D", "H"],
        mux_timer="TIM3",
    ),
    hardware_dir="stm32f446xx",
)

AT32F405XX = Driver(
    platformio=PlatformIO(
        board="genericAT32F405RCT7",
        ldscript="at32f405xc.ld",
        framework="at32firmlib",
        platform="https://github.com/ArteryTek/platform-arterytekat32.git",
    ),
    tinyusb=TinyUSB(mcu="at32f402_405"),
    metadata=Metadata(
        bootloader=Bootloader(
            address=0x1FFFA400,
            magic=0xDEADBEEF,
        ),
        flash=Flash(
            sector_sizes=UniformSectors(
                size=2048,
                num_sectors=128,
            ),
            empty_value=0xFFFFFFFF,
        ),
        adc=ADC(
            max_resolution=12,
            input_pins=[
                "A0",
                "A1",
                "A2",
                "A3",
                "A4",
                "A5",
                "A6",
                "A7",
                "B0",
                "B1",
                "C0",
                "C1",
                "C2",
                "C3",
                "C4",
                "C5",
            ],
            to_gpio_array=lambda pins: (
                [f"GPIO{pin[0]}" for pin in pins],
                [f"GPIO_PINS_{pin[1:]}" for pin in pins],
            ),
            oversample_map=_AT32F405_OVERSAMPLE_MAP,
            sample_cycles_map=_AT32F405_SAMPLE_CYCLES_MAP,
        ),
        valid_ports=["A", "B", "C", "D", "F"],
        encoder_timers=_AT32F405_ENCODER_TIMERS,
        mux_timer="TMR6",
    ),
    hardware_dir="at32f405xx",
)

AT32F405XB = Driver(
    platformio=PlatformIO(
        board="genericAT32F405RBT7-7",
        ldscript="at32f405xb.ld",
        framework="at32firmlib",
        platform="https://github.com/ArteryTek/platform-arterytekat32.git",
    ),
    tinyusb=TinyUSB(mcu="at32f402_405"),
    metadata=Metadata(
        bootloader=Bootloader(
            address=0x1FFFA400,
            magic=0xDEADBEEF,
        ),
        flash=Flash(
            sector_sizes=UniformSectors(
                size=2048,
                num_sectors=64,
            ),
            empty_value=0xFFFFFFFF,
        ),
        adc=ADC(
            max_resolution=12,
            input_pins=[
                "A0",
                "A1",
                "A2",
                "A3",
                "A4",
                "A5",
                "A6",
                "A7",
                "B0",
                "B1",
                "C0",
                "C1",
                "C2",
                "C3",
                "C4",
                "C5",
            ],
            to_gpio_array=lambda pins: (
                [f"GPIO{pin[0]}" for pin in pins],
                [f"GPIO_PINS_{pin[1:]}" for pin in pins],
            ),
            oversample_map=_AT32F405_OVERSAMPLE_MAP,
            sample_cycles_map=_AT32F405_SAMPLE_CYCLES_MAP,
        ),
        valid_ports=["A", "B", "C", "D", "F"],
        encoder_timers=_AT32F405_ENCODER_TIMERS,
        mux_timer="TMR6",
    ),
    hardware_dir="at32f405xx",
)

STM32G431XX = Driver(
    platformio=PlatformIO(
        board="genericSTM32G431CB",
        ldscript="stm32g431cbtx.ld",
        framework="stm32cube",
        platform="ststm32",
    ),
    tinyusb=TinyUSB(mcu="stm32g4"),
    metadata=Metadata(
        bootloader=Bootloader(
            address=0x1FFF0000,
            magic=0xDEADBEEF,
        ),
        flash=Flash(
            sector_sizes=UniformSectors(
                size=2048,
                num_sectors=64,
            ),
            empty_value=0xFFFFFFFF,
        ),
        adc=ADC(
            max_resolution=12,
            input_pins=[
                "A0",
                "A1",
                "A2",
                "A3",
                "A4",
                "A5",
                "A6",
                "A7",
                "B0",
                "B1",
                "B2",
                "B11",
                "B12",
                "B14",
                "C13",
                "C14",
                "C15",
                "F0",
                "F1",
            ],
            to_gpio_array=lambda pins: (
                [f"GPIO{pin[0]}" for pin in pins],
                [f"GPIO_PIN_{pin[1:]}" for pin in pins],
            ),
            oversample_map=_STM32G431_OVERSAMPLE_MAP,
            sample_cycles_map=_STM32G431_SAMPLE_CYCLES_MAP,
        ),
        valid_ports=["A", "B", "C", "F", "G"],
        mux_timer="TIM4",
    ),
    hardware_dir="stm32g431xx",
)

HPM5300XX = Driver(
    tinyusb=TinyUSB(mcu="hpm5300"),
    metadata=Metadata(
        bootloader=Bootloader(
            address=0x00000000,
            magic=0xCAFEBABE,
        ),
        flash=Flash(
            sector_sizes=UniformSectors(
                size=4096,
                num_sectors=256,
            ),
            empty_value=0xFFFFFFFF,
        ),
        adc=ADC(
            max_resolution=16,
            input_pins=[
                "A0", "A1", "A2",  "A3",  "A4",  "A5",  "A6",  "A7",
                "B0", "B1", "B2",  "B3",  "B4",  "B5",  "B6",  "B7",
                "C0", "C1", "C2",  "C3",  "C4",  "C5",  "C6",  "C7",
                "D0", "D1", "D2",  "D3",  "D4",  "D5",  "D6",  "D7",
            ],
            to_gpio_array=lambda pins: (
                [f"GPIO_DI_GPIO{pin[0]}" for pin in pins],
                [int(pin[1:]) for pin in pins],
            ),
            oversample_map={1: None},
        ),
        valid_ports=["A", "B", "C", "D"],
    ),
    platformio=None,
    hardware_dir="hpm5300xx",
)

# Declarative registry mapping family names and specific chip part numbers to Driver instances
DRIVERS: dict[str, Driver] = {
    "stm32f446xx": STM32F446XX,
    "stm32f446ret6": STM32F446XX,
    "at32f405xx": AT32F405XX,
    "at32f405rct7": AT32F405XX,
    "at32f405cct7": AT32F405XX,
    "at32f405kcu7": AT32F405XX,
    "at32f405xb": AT32F405XB,
    "at32f405rbt7": AT32F405XB,
    "at32f405cbt7": AT32F405XB,
    "at32f405kbu7": AT32F405XB,
    "stm32g431xx": STM32G431XX,
    "stm32g431cbt6": STM32G431XX,
    "stm32g431cbu6": STM32G431XX,
    "hpm5300xx": HPM5300XX,
    "hpm5361": HPM5300XX,
}
