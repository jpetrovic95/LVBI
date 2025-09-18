/**
 * @file ina219.c
 * @brief INA219 sensor driver implementation.
 * @details
 *   Initializes the device, restores calibration, and provides mutex-protected
 *   readout routines for voltage, current, power, and shunt voltage.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ina219.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hwa_i2c.h"
#include "compact_config_db.h"
#include <strings.h>
/* * @brief I2C address and register definitions for INA219
 *
 * This section defines the I2C address and register addresses used by the
 * INA219. It also includes configuration settings for the INA219.
 */
#define INA219_ADDRESS 0x40
#define INA219_REG_CONFIG_ADDRESS 0x00
#define INA219_REG_SHUNT_VOLTAGE_ADDRESS 0x01
#define INA219_REG_BUS_VOLTAGE_ADDRESS 0x02
#define INA219_REG_POWER_ADDRESS 0x03
#define INA219_REG_CURRENT_ADDRESS 0x04
#define INA219_REG_CALIBRATION_ADDRESS 0x05

#define INA219_CONFIG_32V_BUS (0x01u << 13)
#define INA219_CONFIG_GAIN_8_320MV (0x03u << 11)
#define INA219_CONFIG_BADC_12BIT_128 (0x0Fu << 7)
#define INA219_CONFIG_SADC_12BIT_128 (0x0Fu << 3)
#define INA219_CONFIG_MODE_CONTINUOUS (0x07u << 0)
#define INA219_CONFIG_BADC_12BIT (0x03u << 7) // 12-bit, no averaging
#define INA219_CONFIG_SADC_12BIT (0x03u << 3) 

#define INA219_MAX_RETRIES 		10
#define INA219_RETRY_DELAY_MS 	5 /* Delay between retries for voltage conversion of 128 samples since per
		 							 datasheet, it can take up to 68.10ms */

#define BATTERY_VOLTAGE_FULL 4.20f
#define BATTERY_VOLTAGE_EMPTY 3.30f
#define OVERCURRENT_THRESHOLD_MA 300.0f
#define VOLTAGE_GLITCH_DELTA_V 0.15f

/* Battery removal detection: if we observe N consecutive valid low bus voltage
 * samples below this threshold, we assume the cell is disconnected and allow
 * the large downward jump (clearing glitch filter state). */
#define BATTERY_REMOVED_THRESHOLD_V     1.0f
#define BATTERY_REMOVED_MIN_COUNT       10  /* consecutive low samples */

typedef struct { float voltage; uint8_t percent; } soc_lut_entry_t;

uint16_t config_reg = INA219_CONFIG_32V_BUS | INA219_CONFIG_GAIN_8_320MV |
					  INA219_CONFIG_BADC_12BIT |
					  INA219_CONFIG_SADC_12BIT |
					  INA219_CONFIG_MODE_CONTINUOUS;
/* Runtime custom LUT (batt_type=CUSTOM). Starts zeroed; populated from batt_lut_raw. */
#define CUSTOM_LUT_MAX_ENTRIES 24
#define GLITCH_ARM_THRESHOLD_V 2.50f

static const char *TAG = "INA219";

static soc_lut_entry_t custom_soc_lut[CUSTOM_LUT_MAX_ENTRIES];
static size_t custom_soc_lut_len = 0;
static uint16_t calibration_value = 0;
static float current_lsb = 0;
static float power_lsb = 0;
static uint32_t overflow_counter = 0UL;
static SemaphoreHandle_t readings_mutex = NULL;
static bool glitch_filter_armed = false; /* becomes true after first stable voltage above arm threshold */
static uint8_t low_voltage_seq = 0;       /* consecutive low-voltage sample counter */
static bool battery_removed = false;      /* latched state until a higher voltage returns */

ina219_config_t ina219_config = {.shunt_resistance = 0.1f,
								 .max_expected_current = 3.2f,
								 .i2c_device_name = "ina219"};

ina219_readings_t ina219_readings = {.voltage_V = 0.0f,
									 .current_mA = 0.0f,
									 .power_mW = 0.0f,
									 .shunt_voltage_mV = 0.0f,
									 .battery_percentage = 0u,
									 .error_flags = 0u};

static float batt_nominal = 3.6f;
static float batt_cutoff  = 3.0f;
static float batt_full    = 4.2f;
static char  batt_type[16] = "CUSTOM"; /* default now CUSTOM to encourage explicit LUT */
static char  batt_lut_raw[256] = {0}; /* serialized custom LUT string */
static float batt_rint_ohm = 0.0f;    /* optional internal resistance; 0 disables compensation */
/*
 * State Of Charge (SoC) LUTs
 * ------------------------------------------------------------
 * Spported multiple Li-Ion profile curves, selectable via batt_type.
 * Idea: keep a "GENERIC" profile (typical 1S Li-ion discharge) and allow
 * a more specific curve (e.g. LIR2032) for chemistries / form factors with
 * noticeably compressed upper plateau.
 *
 * Both LUTs are defined with ABSOLUTE voltages referenced to a nominal
 * cutoff (~3.0–3.3V) and full (~4.20V). If user configured batt_cutoff /
 * batt_full differ, we proportionally re-map measured V into the LUT span.
 *
 * batt_type options (case-insensitive):
 *   GENERIC  -> generic_soc_lut
 *   LIR2032  -> lir2032_soc_lut (high plateau compressed)
 *   (fallback any other) -> linear interpolation only
 *
 * This design keeps code size small and enables future extension (e.g.,
 * 18650, LiFePO4) by simply adding another table + branch.
 */
static const soc_lut_entry_t generic_soc_lut[] = {
    /* Generic 1S Li-Ion typical relaxed profile */
    {3.30f,  0}, {3.40f,  5}, {3.50f, 10}, {3.60f, 25}, {3.70f, 50},
    {3.75f, 65}, {3.80f, 80}, {3.85f, 90}, {3.90f, 96}, {4.00f,100},
    {4.10f,100}, {4.20f,100}
};
/* -------------------------------------------------------------------------- */
/* Static function prototypes                                                 */
/* -------------------------------------------------------------------------- */
/**
 * @brief Write a 16-bit value to an INA219 register over I2C.
 * @param reg Register address.
 * @param val Value to write.
 * @return ESP_OK on success or error code from I2C layer.
 */
static esp_err_t ina219_write_reg(uint8_t reg, uint16_t val);
/**
 * @brief Read a 16-bit value from an INA219 register over I2C.
 * @param reg Register address.
 * @param val Pointer to store read value.
 * @return ESP_OK on success or error code from I2C layer.
 */
static esp_err_t ina219_read_reg(uint8_t reg, uint16_t *val);
/**
 * @brief Convert measured battery voltage to percentage (0-100).
 * @param voltage Cell voltage in volts.
 * @return Percentage (0-100).
 */
static uint8_t ina219_voltage_to_percent(float voltage);
/**
 * @brief Load battery related configuration parameters from persistent storage.
 *
 * Populates internal batt_* globals from persisted keys (batt_full, batt_nominal,
 * batt_cutoff, batt_type) and parses custom LUT if present.
 * @return void
 */
static void ina219_load_battery_params(void);
/**
 * @brief Parse user provided custom LUT (batt_lut) string into runtime table.
 *
 * Expects comma separated voltage:percent pairs (e.g. "3.30:0,3.70:50,4.20:100").
 * Invalid or unordered points are skipped; requires at least 2 valid points.
 * @return void
 */
static void ina219_parse_custom_lut(void);
/**
 * @brief Interpolate SoC from a LUT.
 * @param lut Pointer to first LUT entry.
 * @param len Number of entries.
 * @param v Adjusted voltage.
 * @return Percentage 0-100.
 */
static uint8_t ina219_soc_from_lut(const soc_lut_entry_t *lut, size_t len, float v);

/**
 * @brief Calibrate INA219 based on current configuration (shunt, expected current).
 * @return ESP_OK on success or error code.
 */
static esp_err_t ina219_calibrate(void);

const ina219_readings_t *ina219_get_readings() {
	static ina219_readings_t snapshot;
	if (xSemaphoreTake(readings_mutex, portMAX_DELAY)) {
		snapshot = ina219_readings;
		xSemaphoreGive(readings_mutex);
	}
	return &snapshot;
}

esp_err_t ina219_enable_averaging_adc_mode(bool enable_averaging) {
	if (enable_averaging) {
		config_reg = INA219_CONFIG_32V_BUS | INA219_CONFIG_GAIN_8_320MV |
					 INA219_CONFIG_BADC_12BIT_128 |
					 INA219_CONFIG_SADC_12BIT_128 |
					 INA219_CONFIG_MODE_CONTINUOUS;
	} else {
		config_reg = INA219_CONFIG_32V_BUS | INA219_CONFIG_GAIN_8_320MV |
					 INA219_CONFIG_BADC_12BIT | INA219_CONFIG_SADC_12BIT |
					 INA219_CONFIG_MODE_CONTINUOUS;
	}
	return ina219_write_reg(INA219_REG_CONFIG_ADDRESS, config_reg);
}

esp_err_t ina219_init(bool enable_averaging) {
	
	char i_max_str[16] = "", shunt_str[16] = "";

    // Defaults
    ina219_config.max_expected_current = 3.2f; // A
    ina219_config.shunt_resistance = 0.1f;     // Ohm

    // Read from NVS
	if (config_get_str(CFG_KEY_I_MAX, i_max_str, sizeof(i_max_str)) == ESP_OK) {
        ina219_config.max_expected_current = atof(i_max_str) / 1000.0f; // Convert mA → A
    }

	if (config_get_str(CFG_KEY_SHUNT, shunt_str, sizeof(shunt_str)) == ESP_OK) {
        ina219_config.shunt_resistance = atof(shunt_str);
    }

    if (ina219_config.shunt_resistance <= 0.0f) {
        ina219_config.shunt_resistance = 0.1f; // Safe fallback
	}
	
	ina219_load_battery_params();

    ESP_LOGI(TAG, "INA219: max %.2f A, shunt %.3f Ω",
             ina219_config.max_expected_current, ina219_config.shunt_resistance);

    // Register device
    ESP_RETURN_ON_ERROR(
        hwa_i2c_register_device(ina219_config.i2c_device_name, INA219_ADDRESS),
        TAG, "I2C register failed");

    // Set averaging mode
    ESP_RETURN_ON_ERROR(
        ina219_enable_averaging_adc_mode(enable_averaging),
        TAG, "Write config failed");

    // Mutex for readings
    readings_mutex = xSemaphoreCreateMutex();
    if (readings_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex for readings");
        return ESP_FAIL;
    }

    return ina219_calibrate();
}

esp_err_t ina219_read_current(void) {
	/* Per datasheet:
	   ShuntVoltage_LSB = 10µV; I[A] = Vshunt / Rshunt.
	   Read the SHUNT_VOLTAGE register and compute current via Ohm's law.
	   This avoids dependence on CURRENT register scaling and matches Eq. (4)
	   semantics when combined with programmed calibration. */
	uint16_t raw = 0;
	esp_err_t err = ina219_read_reg(INA219_REG_SHUNT_VOLTAGE_ADDRESS, &raw);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to read shunt voltage register for current calc");
		return err;
	}

	/* Convert raw shunt register (signed, 10µV LSB) to volts */
	float v_shunt_V = (int16_t)raw * 0.00001f; /* 10 microvolts per LSB */
	float current_A = 0.0f;
	if (ina219_config.shunt_resistance > 0.0f) {
		current_A = v_shunt_V / ina219_config.shunt_resistance;
	}
	float current_mA = current_A * 1000.0f;

	if (xSemaphoreTake(readings_mutex, portMAX_DELAY)) {
		if (fabsf(current_mA) > OVERCURRENT_THRESHOLD_MA) {
			ESP_LOGW(TAG, "Over-current detected: %.2f mA", current_mA);
			err_set(&ina219_readings.error_flags, ERR_OVERCURR);
		}
		ina219_readings.current_mA = current_mA;
		xSemaphoreGive(readings_mutex);
	}

	return err;
}

esp_err_t ina219_read_voltage(void) {
	esp_err_t err;
	uint16_t raw = 0;
	bool valid = false;
	bool overflow_detected = false;
	for (int i = 0; i < INA219_MAX_RETRIES; ++i) {
		err = ina219_read_reg(INA219_REG_BUS_VOLTAGE_ADDRESS, &raw);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "Failed to read bus voltage register");
		}
		bool cnvr = (raw >> 1) & 0x01;
		bool ovf = (raw >> 2) & 0x01;

		if (cnvr && !ovf) {
			uint16_t shifted = raw >> 3;
			// Check if the voltage is above a minimum threshold
			if (shifted > 0x0002) {
				valid = true;
				break;
			} else {
				ESP_LOGI(TAG, "Bus Voltage CNVR bit: %d, OVF bit: %d", cnvr,
						 ovf);
				ESP_LOGW(TAG, "Voltage value too low (0x%04X). Retrying...",
						 shifted);
			}
		}
		if (ovf) {
			overflow_counter++;
			overflow_detected = true;
			ESP_LOGW(TAG, "Overflow (OVF) bit set. Discarding value.");
			break;
		}
		if (!cnvr) {
			ESP_LOGI(
				TAG,
				"Voltage conversion not ready (CNVR bit not set). Retrying...");
		}

		vTaskDelay(pdMS_TO_TICKS(INA219_RETRY_DELAY_MS));
	}

	float prev_voltage = ina219_readings.voltage_V;
	float voltage_V = valid ? (((raw & 0xFFF8) >> 3) * 0.004f) : prev_voltage; // Use previous voltage if not valid

	if (valid) {
		/* Battery removal detection logic (runs before glitch filtering). */
		if (voltage_V < BATTERY_REMOVED_THRESHOLD_V) {
			if (low_voltage_seq < 255) low_voltage_seq++;
			/* Set voltage to 0 if <1V for 5 consecutive measurements */
			if (low_voltage_seq >= 5) {
				voltage_V = 0.0f;
			}
			if (!battery_removed && low_voltage_seq >= BATTERY_REMOVED_MIN_COUNT) {
				battery_removed = true;
				glitch_filter_armed = false; /* reset so future re-attach is accepted cleanly */
				/* Flag battery removed */
				err_set(&ina219_readings.error_flags, ERR_BATT_REMOVED);
				ESP_LOGW(TAG, "Battery removal detected after %u low samples (%.3f V)", low_voltage_seq, voltage_V);
			}
		} else {
			/* Voltage recovered above threshold -> treat as re-attach if we previously latched removal */
			if (battery_removed && voltage_V >= (BATTERY_REMOVED_THRESHOLD_V + 0.20f)) {
				ESP_LOGI(TAG, "Battery re-attached detected (%.3f V)", voltage_V);
				battery_removed = false;
				glitch_filter_armed = false; /* re-arm after stable reading logic below */
				err_clear(&ina219_readings.error_flags, ERR_BATT_REMOVED);
			}
			low_voltage_seq = 0; /* reset sequence */
		}
		if (!glitch_filter_armed) {
			/* Accept first stable reading even if it is a large jump (power-up transition).
			   Arm the glitch filter only after we have seen a voltage in a plausible cell range. */
			if (voltage_V >= GLITCH_ARM_THRESHOLD_V) {
				glitch_filter_armed = true;
				ESP_LOGI(TAG, "Glitch filter armed at %.3f V", voltage_V);
			}
		} else if (!battery_removed && prev_voltage > 0.0f && fabsf(voltage_V - prev_voltage) > VOLTAGE_GLITCH_DELTA_V) {
			ESP_LOGW(TAG, "Voltage glitch: prev=%.3fV new=%.3fV (>%.2fV). Using prev.", prev_voltage, voltage_V, VOLTAGE_GLITCH_DELTA_V);
			voltage_V = prev_voltage; /* discard spike */
			valid = false;
		}
	}
	uint8_t percentage = ina219_voltage_to_percent(voltage_V);

	if (!valid) {
		ESP_LOGW(TAG,
				 "Voltage conversion not ready or invalid after retries. Using "
				 "previous voltage: %.3fV",
				 voltage_V);
	}

	if (xSemaphoreTake(readings_mutex, portMAX_DELAY)) {
		ina219_readings.voltage_V = voltage_V;
		ina219_readings.battery_percentage = percentage;
		if (overflow_detected)
			err_set(&ina219_readings.error_flags, ERR_INA_OVF);
		
		xSemaphoreGive(readings_mutex);
	}

	return err;
}

esp_err_t ina219_read_power(void) {
	esp_err_t err;
	uint16_t raw_power = 0;
	err = ina219_read_reg(INA219_REG_POWER_ADDRESS, &raw_power);
	if (err != ESP_OK)
		ESP_LOGE(TAG, "Failed to read power register");

	float power_mW = raw_power * power_lsb * 1000.0f;

	if (xSemaphoreTake(readings_mutex, portMAX_DELAY)) {
		ina219_readings.power_mW = power_mW;
		xSemaphoreGive(readings_mutex);
	}

	return err;
}

esp_err_t ina219_read_shunt_voltage(void) {
	esp_err_t err;
	uint16_t raw;
	err = ina219_read_reg(INA219_REG_SHUNT_VOLTAGE_ADDRESS, &raw);
	if (err != ESP_OK)
		ESP_LOGE(TAG, "Failed to read shunt voltage register");

	//ESP_LOGE(TAG, "Raw Shunt Reg: 0x%04X", raw);
	float shunt_voltage_mV = (int16_t)raw * 0.01f;

	if (xSemaphoreTake(readings_mutex, portMAX_DELAY)) {
		ina219_readings.shunt_voltage_mV = shunt_voltage_mV;
		xSemaphoreGive(readings_mutex);
	}

	return err;
}

err_flag_t ina219_get_error_flags(void) { 
	return ina219_readings.error_flags; 
}

esp_err_t ina219_update_all_readings(void) {
	esp_err_t err;
	ina219_readings.error_flags = ERR_NONE; // Reset error flags before next read

	err = ina219_read_voltage();
	if (err != ESP_OK)
		return err;

	err = ina219_read_current();
	if (err != ESP_OK)
		return err;

	err = ina219_read_power();
	if (err != ESP_OK)
		return err;

	ina219_readings.battery_percentage = ina219_voltage_to_percent(ina219_readings.voltage_V);

	err = ina219_read_shunt_voltage();

	return err;
}

static esp_err_t ina219_write_reg(uint8_t reg, uint16_t val) {
	uint8_t data[3] = {reg, val >> 8, val & 0xFF};
	esp_err_t e = hwa_i2c_write(ina219_config.i2c_device_name, data, 3);
	if (e != ESP_OK) ESP_LOGE(TAG, "I2C bus error during init: %s", esp_err_to_name(e));
	return e;
}

static esp_err_t ina219_read_reg(uint8_t reg, uint16_t *val) {
	esp_err_t err = hwa_i2c_write(ina219_config.i2c_device_name, &reg, 1);
	if (err != ESP_OK)
		{ ESP_LOGE(TAG, "I2C write failed: %s", esp_err_to_name(err)); return err; }

	uint8_t buffer[2];
	err = hwa_i2c_read(ina219_config.i2c_device_name, buffer, 2);
	if (err != ESP_OK)
		{ ESP_LOGE(TAG, "I2C write failed: %s", esp_err_to_name(err)); return err; }

	*val = (buffer[0] << 8) | buffer[1];
	/* success: any previous local bus error is considered cleared */
	return ESP_OK;
}

static uint8_t ina219_voltage_to_percent(float voltage) {
	if (batt_nominal == 0 || batt_cutoff == 0 || batt_full == 0) {
		ina219_load_battery_params();
	}
	if (voltage >= batt_full) return 100;
	if (voltage <= batt_cutoff) return 0;

	/* Optional OCV compensation: approximate open-circuit voltage by adding |I|*Rint */
	float v_for_soc = voltage;
	if (batt_rint_ohm > 0.0f) {
		float i_abs_A = fabsf(ina219_readings.current_mA) / 1000.0f; /* mA -> A */
		/* Apply compensation only if current magnitude is meaningful to avoid noise amplification */
		if (i_abs_A > 0.002f) { /* >2 mA */
			v_for_soc = voltage + (i_abs_A * batt_rint_ohm);
		}
		/* Bound to sane range to avoid excessive inflation */
		if (v_for_soc > batt_full + 0.05f) v_for_soc = batt_full + 0.05f;
		if (v_for_soc < batt_cutoff - 0.10f) v_for_soc = batt_cutoff - 0.10f;
	}

	/* Select LUT (if any) */
	const soc_lut_entry_t *lut = NULL; size_t lut_len = 0;
	if (strcasecmp(batt_type, "CUSTOM") == 0) {
		if (custom_soc_lut_len > 1) { lut = custom_soc_lut; lut_len = custom_soc_lut_len; }
	} else if (strcasecmp(batt_type, "GENERIC") == 0 || strcasecmp(batt_type, "LIION") == 0) {
		lut = generic_soc_lut;
		lut_len = sizeof(generic_soc_lut)/sizeof(generic_soc_lut[0]);
	}

	if (lut) {
		float lut_min = lut[0].voltage;
		float lut_max = lut[lut_len-1].voltage;
		float v_adj = v_for_soc;
		
		if (fabsf(batt_cutoff - lut_min) > 0.01f || fabsf(batt_full - lut_max) > 0.01f) {
			float span_user = batt_full - batt_cutoff;
			float span_lut  = lut_max - lut_min;
			if (span_user > 0.0f) {
			    float norm = (v_for_soc - batt_cutoff) / span_user;
				if (norm < 0) norm = 0;
				if (norm > 1) norm = 1;
				v_adj = lut_min + norm * span_lut;
			}
		}
		return ina219_soc_from_lut(lut, lut_len, v_adj);
	}

	/* Fallback: simple linear mapping */
    float soc = (v_for_soc - batt_cutoff) / (batt_full - batt_cutoff) * 100.0f;
	if (soc < 0) soc = 0;
	if (soc > 100) soc = 100;
	return (uint8_t)(soc + 0.5f);
}

static esp_err_t ina219_calibrate(void) {
	current_lsb = ina219_config.max_expected_current / 32768.0f;
	current_lsb = ceilf(current_lsb * 1e6f) / 1e6f;
	calibration_value = (uint16_t)(0.04096 / (current_lsb * ina219_config.shunt_resistance)) & 0xFFFE;

	esp_err_t err = ina219_write_reg(INA219_REG_CONFIG_ADDRESS, config_reg);
	if (err != ESP_OK)
		ESP_LOGE(TAG, "Failed to write config register");

	err = ina219_write_reg(INA219_REG_CALIBRATION_ADDRESS, calibration_value);
	if (err != ESP_OK)
		ESP_LOGE(TAG, "Failed to write calibration register");

	power_lsb = current_lsb * 20.0f;

	ESP_LOGI(TAG,
			 "INA219 calibrated: CAL=0x%04X, CurrentLSB=%.6f, PowerLSB=%.6f",
			 calibration_value, current_lsb, power_lsb);

	uint16_t config_verify;
	if (ina219_read_reg(INA219_REG_CONFIG_ADDRESS, &config_verify) == ESP_OK) {
		if (config_verify != config_reg) {
			ESP_LOGW(TAG,
					 "Config register mismatch: expected 0x%04X, got 0x%04X",
					 config_reg, config_verify);
		}
		ESP_LOGI(TAG, "Config Register: 0x%04X", config_verify);
	} else {
		ESP_LOGE(TAG, "Failed to read config register");
	}

	uint16_t cal_verify;

	if (ina219_read_reg(INA219_REG_CALIBRATION_ADDRESS, &cal_verify) == ESP_OK) {
		if (cal_verify != calibration_value) {
			ESP_LOGW(
				TAG,
				"Calibration register mismatch: expected 0x%04X, got 0x%04X",
				calibration_value, cal_verify);
		}
		ESP_LOGI(TAG, "Calibration Register: 0x%04X", cal_verify);
	} else {
		ESP_LOGE(TAG, "Failed to read calibration register");
	}

	vTaskDelay(pdMS_TO_TICKS(10));

	return err;
}

static void ina219_load_battery_params(void)
{
	/* Use consolidated battery config loader */
	battery_cfg_t bc; 
	config_load_battery_cfg(&bc);
	batt_full    = bc.full;
	batt_nominal = bc.nominal;
	batt_cutoff  = bc.cutoff;
	batt_rint_ohm= bc.rint_ohm;
	strlcpy(batt_type, bc.type, sizeof(batt_type));
	/* Custom LUT raw string (optional) */
	config_get_str(CFG_KEY_BATT_LUT, batt_lut_raw, sizeof(batt_lut_raw));
	ina219_parse_custom_lut();
}

static void ina219_parse_custom_lut(void) {
	custom_soc_lut_len = 0;
	if (batt_lut_raw[0] == '\0') return; /* nothing */
	memset(custom_soc_lut, 0, sizeof(custom_soc_lut));
	char buf[sizeof(batt_lut_raw)];
	strncpy(buf, batt_lut_raw, sizeof(buf));
	buf[sizeof(buf)-1] = '\0';
	char *saveptr = NULL;
	char *tok = strtok_r(buf, ",", &saveptr);
	float prev_v = 0.0f;
	while (tok && custom_soc_lut_len < CUSTOM_LUT_MAX_ENTRIES) {
		char *sep = strchr(tok, ':');
		if (!sep) sep = strchr(tok, '=');
		if (sep) {
			*sep = '\0';
			const char *v_str = tok;
			const char *p_str = sep + 1;
			float v = strtof(v_str, NULL);
			int p = atoi(p_str);
			if (v >= 2.5f && v <= 4.35f && p >= 0 && p <= 100) {
				if (custom_soc_lut_len == 0 || v > prev_v + 1e-4f) {
					custom_soc_lut[custom_soc_lut_len].voltage = v;
					custom_soc_lut[custom_soc_lut_len].percent = (uint8_t)p;
					prev_v = v;
					custom_soc_lut_len++;
				}
			}
		}
		tok = strtok_r(NULL, ",", &saveptr);
	}
	if (custom_soc_lut_len >= 2) {
		ESP_LOGI(TAG, "Custom LUT loaded (%d points)", (int)custom_soc_lut_len);
	} else {
		ESP_LOGW(TAG, "Custom LUT invalid or too few points (len=%d). Ignoring.", (int)custom_soc_lut_len);
		custom_soc_lut_len = 0;
	}
}

static uint8_t ina219_soc_from_lut(const soc_lut_entry_t *lut, size_t len, float v) {
	if (v <= lut[0].voltage) return lut[0].percent;
	if (v >= lut[len-1].voltage) return lut[len-1].percent;
	for (size_t i = 1; i < len; i++) {
		if (v <= lut[i].voltage) {
			const soc_lut_entry_t *a = &lut[i-1];
			const soc_lut_entry_t *b = &lut[i];
			float span = b->voltage - a->voltage;
			if (span <= 0) return b->percent;
			float t = (v - a->voltage) / span;
			if (t < 0) t = 0;
			if (t > 1) t = 1;
			float p = a->percent + t * (b->percent - a->percent);
			if (p < 0) p = 0;
			if (p > 100) p = 100;
			return (uint8_t)(p + 0.5f);
		}
	}
	return 0;
}
