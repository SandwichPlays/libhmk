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

from enum import Enum
from pydantic import (
    BaseModel,
    Field,
    NonNegativeInt,
    PositiveFloat,
    PositiveInt,
    model_validator,
)


class SwitchDefinition(BaseModel):
    id: str
    name: str
    manufacturer: str = "Generic"
    travel_mm: PositiveFloat = 3.6
    initial_bottom_out_threshold: NonNegativeInt = 600
    lut: list[NonNegativeInt] = Field(min_length=1024, max_length=1024)


class KeyboardUSBPort(str, Enum):
    FULL_SPEED = "fs"
    HIGH_SPEED = "hs"


class KeyboardUSB(BaseModel):
    vid: str = Field(pattern=r"^0x[0-9A-Fa-f]{4}$")
    pid: str = Field(pattern=r"^0x[0-9A-Fa-f]{4}$")
    port: KeyboardUSBPort = KeyboardUSBPort.HIGH_SPEED


class KeyboardHallMux(BaseModel):
    # Multiplexer select GPIO pins (e.g. ["C13", "C14", "C15"])
    select: list[str]
    # ADC input pins connected to each multiplexer output
    pins: list[str]
    # 0-indexed key mapping [mux_idx][channel_idx] (null for unconnected channels)
    matrix: list[list[NonNegativeInt | None]]


class KeyboardHall(BaseModel):
    # Direct ADC input pins in 0-indexed key order (or mapped via `keys`)
    pins: list[str] | None = None
    # Optional 0-indexed key mapping for direct ADC pins (defaults to 0..N-1)
    keys: list[NonNegativeInt | None] | None = None
    # Analog multiplexer configuration
    mux: KeyboardHallMux | None = None
    # Hardware oversampling ratio (1, 2, 4, 8, 16, 32, 64, 128, 256)
    oversample: PositiveInt | None = None
    # ADC sample time in cycles (e.g. 1.5, 7.5, 13.5, 28.5 on AT32)
    sample_cycles: PositiveFloat | None = None
    # ADC resolution override
    adc_resolution: PositiveInt | None = None
    # Invert raw ADC reading globally
    invert_adc: bool = False
    # Settling delay in microseconds between mux channel switches
    mux_delay_us: int | None = None
    # Initial rest ADC estimate prior to auto-zeroing
    rest_adc: NonNegativeInt = 2400
    # Optional board-level override for initial bottom-out delta threshold
    bottom_out_threshold: NonNegativeInt | None = None
    # Optional board-level override for switch travel in mm
    travel_mm: PositiveFloat | None = None

    @model_validator(mode="after")
    def populate_direct_keys(self):
        if self.pins is not None and self.keys is None:
            self.keys = list(range(len(self.pins)))
        return self


class KeyboardDigitalDirect(BaseModel):
    pins: list[str]
    # 0-indexed key numbers
    keys: list[NonNegativeInt]
    active_low: bool = True
    debounce_ms: NonNegativeInt = 5


class KeyboardDigitalMatrix(BaseModel):
    rows: list[str]
    cols: list[str]
    diode_direction: str = Field(default="col2row", pattern=r"^(col2row|row2col)$")
    debounce_ms: NonNegativeInt = 5
    # 0-indexed key numbers (null for empty positions)
    matrix: list[list[NonNegativeInt | None]]


class KeyboardDigital(BaseModel):
    direct: KeyboardDigitalDirect | None = None
    matrix: KeyboardDigitalMatrix | None = None


class KeyboardEncoder(BaseModel):
    # Quadrature A/B GPIO pins (works on any GPIO pins; uses hardware timer if supported)
    pins: list[str] = Field(min_length=2, max_length=2)
    # 0-indexed virtual keys for [CCW, CW] rotation
    keys: list[NonNegativeInt] = Field(min_length=2, max_length=2)
    # Optional timer override ("TMR1".."TMR4" or "GPIO")
    timer: str | None = None
    # Steps per detent
    resolution: PositiveInt = 4
    # Optional push-button GPIO pin and 0-indexed virtual key
    button_pin: str | None = None
    button_key: NonNegativeInt | None = None
    # Physical layout coordinates in the web configurator
    x: float = 0.0
    y: float = 0.0
    w: PositiveFloat = 1.0
    h: PositiveFloat = 1.0


class KeyboardTuning(BaseModel):
    ema_alpha_numerator: PositiveInt | None = None
    ema_alpha_shift: PositiveInt | None = None
    hysteresis_gap_um: NonNegativeInt | None = None
    calibration_epsilon: NonNegativeInt | None = None
    baseline_down_ms: PositiveInt | None = None
    rest_lenience_um: NonNegativeInt | None = None
    bottom_lenience_um: NonNegativeInt | None = None


class KeyboardRGB(BaseModel):
    pin: str
    type: str = Field(default="ws2812", pattern=r"^(ws2812|sk6812)$")
    num_leds: PositiveInt
    timer: str = "TMR1"
    channel: int = Field(default=1, ge=1, le=4)
    dma_channel: int = Field(default=2, ge=2, le=7)
    max_current_ma: PositiveInt = 400
    key_map: list[NonNegativeInt] | None = None


class KeyboardWearLeveling(BaseModel):
    virtual_size: int = Field(ge=1, le=32768)
    write_log_size: int = Field(ge=1, le=65536)


class KeyboardLayoutKey(BaseModel):
    key: NonNegativeInt
    w: PositiveFloat | None = None
    h: PositiveFloat | None = None
    x: float | None = None
    y: float | None = None
    option: tuple[int, int] | None = None


class KeyboardLayout(BaseModel):
    labels: list[str | list[str]] | None = None
    keys: list[list[KeyboardLayoutKey]]


class KeyboardActuation(BaseModel):
    actuation_point_um: int | None = Field(default=None, ge=50, le=4000)
    rt_sensitivity_um: int | None = Field(default=None, ge=0, le=4000)
    rt_deadzone_um: int | None = Field(default=None, ge=0, le=4000)


class Keyboard(BaseModel):
    name: str
    manufacturer: str
    maintainer: str
    mcu: str
    crystal_hz: PositiveInt = 12000000
    usb: KeyboardUSB
    switch: str = "yingying_dual_lights_max"
    num_profiles: int = Field(default=4, ge=1, le=8)
    num_layers: int | None = Field(default=None, ge=1, le=8)
    num_keys: int | None = Field(default=None, ge=1, le=256)
    num_advanced_keys: int | None = Field(default=None, ge=1, le=64)
    hall: KeyboardHall = Field(default_factory=KeyboardHall)
    digital: KeyboardDigital | None = None
    encoders: list[KeyboardEncoder] | None = None
    rgb: KeyboardRGB | None = None
    tuning: KeyboardTuning | None = None
    actuation: KeyboardActuation | None = None
    wear_leveling: KeyboardWearLeveling | None = None
    layout: KeyboardLayout
    keymap: list[list[str]] | None = None
    keymaps: list[list[list[str]]] | None = None

    @model_validator(mode="after")
    def populate_smart_defaults(self):
        ref_keymap = self.keymap or (self.keymaps[0] if self.keymaps else None)
        if self.num_keys is None:
            if ref_keymap and len(ref_keymap) > 0:
                self.num_keys = len(ref_keymap[0])
            else:
                raise ValueError("num_keys must be specified when keymap is empty")
        if self.num_layers is None:
            if ref_keymap and len(ref_keymap) > 0:
                self.num_layers = max(4, len(ref_keymap))
            else:
                self.num_layers = 4
        if self.num_advanced_keys is None:
            self.num_advanced_keys = min(max(self.num_keys, 8), 32)
        return self
