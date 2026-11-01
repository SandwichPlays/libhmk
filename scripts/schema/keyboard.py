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


# USB Configuration
class KeyboardUSB(BaseModel):
    # USB Vendor ID
    vid: str = Field(pattern=r"^0x[0-9A-Fa-f]{4}$")
    # USB Product ID
    pid: str = Field(pattern=r"^0x[0-9A-Fa-f]{4}$")
    port: KeyboardUSBPort = KeyboardUSBPort.HIGH_SPEED


# Keyboard Configuration
class KeyboardKeyboard(BaseModel):
    num_profiles: int = Field(default=4, ge=1, le=8)
    num_layers: int | None = Field(default=None, ge=1, le=8)
    num_keys: int | None = Field(default=None, ge=1, le=256)
    num_advanced_keys: int | None = Field(default=None, ge=1, le=64)


# Hardware Configuration
class KeyboardHardware(BaseModel):
    # High-speed external oscillator value in Hz
    hse_value: PositiveInt = 12000000
    # Keyboard driver or MCU part number
    driver: str


# Raw ADC input configuration
class KeyboardAnalogRaw(BaseModel):
    # Array of raw ADC input channels. If a string is provided, it is used as the GPIO pin name
    input: list[NonNegativeInt | str]
    # Array of key index mappings for the raw ADC inputs (defaults to 1..N if omitted)
    vector: list[NonNegativeInt] | None = None

    @model_validator(mode="after")
    def populate_default_vector(self):
        if self.vector is None:
            self.vector = list(range(1, len(self.input) + 1))
        return self


# Analog Multiplexer ADC input configuration
class KeyboardAnalogMux(BaseModel):
    # Array of GPIO pin names for the multiplexer select lines
    select: list[str]
    # Array of ADC input channels for the multiplexers. If a string is provided, it is used as the GPIO pin name
    input: list[NonNegativeInt | str]
    # Mapping from multiplexers to ADC input channels
    matrix: list[list[NonNegativeInt]]


# Direct GPIO digital key configuration
class KeyboardDigitalDirect(BaseModel):
    pins: list[str]
    keys: list[NonNegativeInt]
    active_low: bool = True
    debounce_ms: NonNegativeInt = 5


# Diode matrix digital key configuration
class KeyboardDigitalMatrix(BaseModel):
    rows: list[str]
    cols: list[str]
    diode_direction: str = Field(default="col2row", pattern=r"^(col2row|row2col)$")
    debounce_ms: NonNegativeInt = 5
    matrix: list[list[NonNegativeInt]]


# Digital / Mechanical switch configuration
class KeyboardDigital(BaseModel):
    direct: KeyboardDigitalDirect | None = None
    matrix: KeyboardDigitalMatrix | None = None


# Rotary Encoder configuration
class KeyboardEncoder(BaseModel):
    # Array of GPIO pin names for the encoder pins
    pins: list[str] = Field(min_length=2, max_length=2)
    # The two virtual keys for CCW and CW (1-indexed key numbers)
    keys: list[NonNegativeInt] = Field(min_length=2, max_length=2)
    # Optional hardware timer ("TMR1".."TMR4" or "GPIO"); auto-detected from pins if omitted
    timer: str | None = None
    # The number of steps per detent
    resolution: PositiveInt = 4
    # The GPIO pin for the encoder button
    button_pin: str | None = None
    # The virtual key for the encoder button (1-indexed key number)
    button_key: NonNegativeInt | None = None


# Analog Configuration
class KeyboardAnalog(BaseModel):
    # ADC resolution for this keyboard. Defaults to the maximum resolution supported by the MCU if not provided.
    adc_resolution: PositiveInt | None = None
    # Set to true if the ADC value is inversely proportional to the travel distance of the keys
    invert_adc: bool = False
    # Delay in microseconds between ADC scans (for mux settling)
    delay: int | None = None
    # Hardware oversampling ratio (1, 2, 4, 8, 16, 32, 64, 128, 256)
    oversample: PositiveInt | None = None
    # ADC sample time in cycles (e.g. 1.5, 7.5, 13.5, 28.5 on AT32)
    sample_cycles: PositiveFloat | None = None
    raw: KeyboardAnalogRaw | None = None
    mux: KeyboardAnalogMux | None = None


class KeyboardCalibration(BaseModel):
    # Initial rest ADC estimate prior to auto-zeroing
    initial_rest_value: NonNegativeInt = 2000
    # Initial bottom-out delta threshold (defaults to switch file if omitted)
    initial_bottom_out_threshold: NonNegativeInt | None = None
    # Total switch travel distance in millimeters (defaults to switch file if omitted)
    switch_travel: float | None = None


# Filter & Calibration Tuning Configuration
class KeyboardTuning(BaseModel):
    ema_alpha_numerator: PositiveInt | None = None
    ema_alpha_shift: PositiveInt | None = None
    hysteresis_gap_um: NonNegativeInt | None = None
    calibration_epsilon: NonNegativeInt | None = None
    baseline_down_ms: PositiveInt | None = None
    rest_lenience_um: NonNegativeInt | None = None
    bottom_lenience_um: NonNegativeInt | None = None


# RGB Lighting Configuration (WS2812 / SK6812)
class KeyboardRGB(BaseModel):
    pin: str
    type: str = Field(default="ws2812", pattern=r"^(ws2812|sk6812)$")
    num_leds: PositiveInt
    timer: str = "TMR1"
    channel: int = Field(default=1, ge=1, le=4)
    dma_channel: int = Field(default=2, ge=2, le=7)
    max_current_ma: PositiveInt = 400
    key_map: list[NonNegativeInt] | None = None


# Wear leveling Configuration
class KeyboardWearLeveling(BaseModel):
    # Size of the virtual persistent storage in bytes.
    virtual_size: int = Field(ge=1, le=32768)
    # Size of the write log in bytes
    write_log_size: int = Field(ge=1, le=65536)


# Keyboard Layout Encoder
class KeyboardLayoutEncoder(BaseModel):
    # The two virtual keys for CCW and CW
    keys: list[NonNegativeInt] = Field(min_length=2, max_length=2)
    buttonKey: NonNegativeInt | None = None
    x: float
    y: float
    w: PositiveFloat | None = None
    h: PositiveFloat | None = None


class KeyboardLayoutKey(BaseModel):
    key: NonNegativeInt
    w: PositiveFloat | None = None
    h: PositiveFloat | None = None
    x: float | None = None
    y: float | None = None
    # Option key and value pairs for the corresponding labels
    option: tuple[int, int] | None = None


# Keyboard Layout
class KeyboardLayout(BaseModel):
    # Labels for each layout option. Use a string for a toggle option, or an array for a select option
    labels: list[str | list[str]] | None = None
    # Metadata for how the keyboard should be rendered in the web configurator
    keymap: list[list[KeyboardLayoutKey]]
    encoders: list[KeyboardLayoutEncoder] | None = None


# Actuation Configuration
class KeyboardActuation(BaseModel):
    # Default actuation point (legacy 0..255 or micrometers if > 255)
    actuation_point: int | None = Field(default=None, ge=0, le=4000)
    # Default actuation point in micrometers (0.001mm)
    actuation_point_um: int | None = Field(default=None, ge=50, le=4000)
    # Default Rapid Trigger sensitivity in micrometers (0 = disabled)
    rt_sensitivity_um: int | None = Field(default=None, ge=0, le=4000)
    # Default Rapid Trigger deadzone in micrometers
    rt_deadzone_um: int | None = Field(default=None, ge=0, le=4000)


# keyboard.json Schema
class Keyboard(BaseModel):
    name: str
    manufacturer: str
    maintainer: str
    usb: KeyboardUSB
    keyboard: KeyboardKeyboard = Field(default_factory=KeyboardKeyboard)
    hardware: KeyboardHardware
    switch: str = "yingying_dual_lights_max"
    analog: KeyboardAnalog = Field(default_factory=KeyboardAnalog)
    digital: KeyboardDigital | None = None
    calibration: KeyboardCalibration = Field(default_factory=KeyboardCalibration)
    tuning: KeyboardTuning | None = None
    rgb: KeyboardRGB | None = None
    wear_leveling: KeyboardWearLeveling | None = None
    encoders: list[KeyboardEncoder] | None = None
    layout: KeyboardLayout
    # Default keymap
    keymap: list[list[str]] | None = None
    # Default keymaps for each profile. If not specified, the default keymap will be used for all profiles.
    keymaps: list[list[list[str]]] | None = None
    actuation: KeyboardActuation | None = None

    @model_validator(mode="after")
    def populate_smart_defaults(self):
        ref_keymap = self.keymap or (self.keymaps[0] if self.keymaps else None)
        if self.keyboard.num_keys is None:
            if ref_keymap and len(ref_keymap) > 0:
                self.keyboard.num_keys = len(ref_keymap[0])
            else:
                raise ValueError("keyboard.num_keys must be specified when keymap is empty")
        if self.keyboard.num_layers is None:
            if ref_keymap and len(ref_keymap) > 0:
                self.keyboard.num_layers = max(4, len(ref_keymap))
            else:
                self.keyboard.num_layers = 4
        if self.keyboard.num_advanced_keys is None:
            self.keyboard.num_advanced_keys = min(self.keyboard.num_keys, 32)
        return self
