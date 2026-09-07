/* Port of the "custom firmware configuration" part of pvvx TelinkMiFlasher.html v14.1
   (https://pvvx.github.io/ATC_MiThermometer/TelinkMiFlasher.html, (c) pvvx / atc1441).
   The BLE transport (Web Bluetooth characteristic) is replaced by the ESP32 bridge: bleWrite(bytes) -> Promise,
   and notifications from 0x1F1F are delivered to CustomBlkParse(DataView) by app.js.  Everything else is kept
   verbatim so the tab looks and behaves exactly like the original. */
/* eslint-disable */
var $ = function(id) { return document.getElementById(id);}
var settingsCharacteristics = null; // truthy while the ESP32 holds a connection; .writeValue is provided by app.js
var cfg = {enable: false, zbdevice: false, ver: 0, hver: 0, ext_hw_id: 0, flg: 0, flg2: 0, temp_offset: 0, humi_offset: 0, advertising_interval: 32, measure_interval: 5, rf_tx_power: 191, connect_latency: 49, lcd_tint: 55, av_meas_mem: 60, step_time: 16000000};
var trg = {enable: false, tmp_thr: 2100, hm_thr: 5000, tmp_hst: -1, hm_hst: 0, flg: 0, rds_type: 0, rds_rpint: 0};
var cmf = {enable: false, tmp_lo: 2100, tmp_hi: 2600, hm_lo: 3000, hm_hi: 6000};
var dnm = {enable: false, name: ""};
var devtime = {};
var pincode = {enable: false, value: 0};
var devinfo = { hrstr: null, srstr: null, frstr: null };
var trsc = {int: null, mac: null, mact: null};
var devSens = {};
var hwver_id = null;
var bigOtaEnabled = false, devTest = false, bthomeEnabled = false, fwmaxsize = 0x20000, flg_memo_act = false;
var MAX_EXT_OTA_SIZE = 0x34000;
function addLog(logTXT) { if (window.xfLog) window.xfLog(logTXT); }
function addClog(logTXT) { console.log(logTXT); }
function addAlog(logTXT) { console.log(logTXT); addLog(logTXT); }
function setStatus(status) { var e = $("percent"); if (e) e.innerHTML = "Status: " + status; }
function menuUpgrade() { if (window.xfMenuUpgrade) window.xfMenuUpgrade(); }
function decimalToHex(d, padding) { var hex = Number(d).toString(16); while (hex.length < 4) hex = "0" + hex; return hex; }
function hexToBytes(hex) { for (var bytes = [], c = 0; c < hex.length; c += 2) bytes.push(parseInt(hex.substr(c, 2), 16)); return new Uint8Array(bytes); }
function bytesToHex(data) { return new Uint8Array(data).reduce(function(memo, i) { return memo + ("0" + i.toString(16)).slice(-2); }, ""); }
function hex2ascii(hexx) { var hex = hexx.toString(); var str = ''; for (var i = 0; (i < hex.length && hex.substr(i, 2) !== '00'); i += 2) str += String.fromCharCode(parseInt(hex.substr(i, 2), 16)); return str; }
function sendCustomSetting(data){
	if((hexToBytes(data))[0] == 0x35) flg_memo_act = true;
	settingsCharacteristics.writeValue(hexToBytes(data)).then(function() {
		addLog("Settings " + data + " was sent successfully");
	}).catch(function(err) {
		addLog("Error on sending setting " + data);
	});
}

const hw_version_str = [
'LYWSD03MMC', // B1.4
'MHO-C401(old)',
'CGG1-M(2020,2021)',
'LYWSD03MMC', // B1.9
'LYWSD03MMC', // B1.6
'LYWSD03MMC', // B1.7|B2.0',
'CGDK2',
'CGG1-M(2022)',
'MHO-C401(2022)',
'MJWSD05MMC(ch)',
'LYWSD03MMC', // B1.5
'MHO-C122',
'MJWSD05MMC(en)',
'MJWSD06MMC',
'LYWSD03MMC', // B1.6/B1.1(2025)
'ID15',
'TB03F',
'TS0201',
'TNKS',
'THB2',
'BTH01',
'TH05',
'TH03Z',
'THB1',
'TH05D',
'TH05F',
'THB3',
'ZTH01',
'ZTH02',
'PLM1',
'TH03(DIY)',
'LKTMZL02',
'KEY2',
'ZTH05',
'TH04',
'CB3S',
'HS09',
'ZY-ZTH02',
'ZY-ZTH02/03-Pro', // ZY-ZTH03Pro
'ZG-227Z',
'TS0202_PIR1',
'TS0202_PIR2',
'HDP16',
'TN_6ATAG3',
'ZG-303Z',
'ZBeacon-TH01',
'ZBeaconMC',
'ZBeaconMC2',
'RSH_HS03',
'LYWSD02MMC',
'ZG204ZL',
'ZG204ZV',
'TS0201_WING',
'DIY-SCD41'];

const id_sensor_str = [
"None",
"SHTV3(C3)",
"SHT4x",
"SHT30",
"CHT8305",
"ANT20/30",
"CHT8215",
"INA226",
"MY18B20",
"MY18B20x2",
"HX71X",
"PWMRH",
"NTC",
"INA3221",
"SCD41",
"BME280"
]

var buf11;
var cnt_buf11 = 0;
var cnt_delkeys = 0;
var id_buf11 = 0x1000;
var mikeys = {mac: null, id: null, token: null, bindkey: null, cfg: null, delkeys: null, restore: false, cbindkey: null};
var ext = {big_number: 0, small_number: 0, vtime: 60, flg: 0xc7, enable: false};

function concatUint8ArrayArrays(a, b) { // a, b TypedArray of same type
	let c = new Uint8Array(a.byteLength + b.byteLength);
	c.set(a, 0);
	c.set(b, a.byteLength);
	return c;
}
function dump(ar, len) {
	let s = '';
	for(let i=0; i < len; i++) {
		s += hex(ar[i],2);
	}
	return s;
}
var atc_mac, atc_rmac;
function CustomBlkParse(value) {
	let len = value.byteLength;
	let s = '';
	if(len == 0) return;
	len--;	// size from cmd
	let blkid = value.getUint8(0);
	if(devTest) {
		s = 'Custom (0xffe1) Notifications id: 0x'+hex(blkid,2)+' ['+bytesToHex(value.buffer.slice(1))+']';
		addAlog(s);
		return;
	}
	if(blkid == 0x55 && len >= 9) {
		cfg.ver = value.getUint8(1);
		cfg.flg = value.getUint8(2);
		cfg.flg2 = value.getUint8(3);
		cfg.temp_offset = value.getInt8(4);
		cfg.humi_offset = value.getInt8(5);
		cfg.advertising_interval = value.getUint8(6);
		cfg.measure_interval = value.getUint8(7);
		cfg.rf_tx_power = value.getUint8(8);
		//if((cfg.rf_tx_power &0x80)==0) MAX_RF_TX_Power=true;
		cfg.connect_latency = value.getUint8(9);
		if(len >= 10) cfg.lcd_tint = value.getUint8(10);
		else cfg.lcd_tint = 55;
		if(len >= 11) {
			cfg.hver = value.getUint8(11);
			if(cfg.ver < 0x36) cfg.hver &= 0x87;
		} else if(cfg.hver == null) cfg.hver = 0x80;
		if(len >= 12) cfg.av_meas_mem = value.getUint8(12); else cfg.av_meas_mem = 0;
		let s = 'Hardware Version: ';
		if(cfg.ver < 0x48) {
			hwver_id = cfg.hver & 0x0F;
			if((len >= 11)&&(hwver_id == 0x0F))  {
				cfg.ext_hw_id = cfg.lcd_tint & 0x7f;
				hwver_id = 16 + cfg.ext_hw_id;
				if((cfg.lcd_tint & 0x80) != 0) bigOtaEnabled = true;
			}
		} else
			hwver_id = cfg.hver;
		if(hwver_id < hw_version_str.length) {
			s += hw_version_str[hwver_id];
		} else {
			s += 'Unknown or DIY ('+hwver_id+')';
		}
		if(cfg.ver > 0x45) bigOtaEnabled = true;
		else if(hwver_id == 9 || hwver_id == 11 || hwver_id == 12 || hwver_id == 13) bigOtaEnabled = true; 
		if(devinfo.hrstr) s += ' ' + devinfo.hrstr;
		s += ', Software Version: '+(cfg.ver>>4)+'.'+(cfg.ver&0x0f);
		setStatus(s);
		addAlog(s);
		addAlog('Custom config HEX string: 55' + bytesToHex(value.buffer.slice(2)));
		if(cfg.ver >= 5) CustomConfig();
		else addLog('Old version - out of service!');
	} else if((blkid == 0x57 || blkid == 0x58) && len >= 5) { // Get/Set zb device config
		cfg.temp_offset = value.getInt16(1, true);
		cfg.humi_offset = value.getInt16(3, true);
		if(len >= 16) {
			cfg.temp_comfort_min = value.getInt16(5, true);
			cfg.temp_comfort_max = value.getInt16(7, true);
			cfg.humi_comfort_min = value.getInt16(9, true);
			cfg.humi_comfort_max = value.getInt16(11, true);
			cfg.c_f = value.getUint8(13);	   
			cfg.showSmiley = value.getUint8(14);
			cfg.display_off = value.getUint8(15);
			cfg.measure_interval = value.getUint8(16);
		} else {
			cfg.measure_interval = value.getUint8(5);
		}
		if(cfg.zbdevice) {
			addAlog('Custom config HEX string: 57' + bytesToHex(value.buffer.slice(2)));
			zbdeviceConfig();
		}
	} else if(blkid == 0 && len >= 11) { // Get dev id, version, services
		cfg.revision  = value.getUint8(1); // protocol version/revision
		cfg.hw_version = value.getUint16(2, true); // hardware version
		cfg.sw_version  = value.getUint16(4, true);	// software version (BCD)
		cfg.dev_spec_data  = value.getUint16(6, true); // device-specific data (bit0..3: sensor_type)
		cfg.services  = value.getUint32(8, true); // supported services by the device
		let s = " ?";
		if(cfg.hw_version < hw_version_str.length) {
			hwver_id = cfg.hw_version;
			s = " " + hw_version_str[cfg.hw_version];
			if(devinfo.hrstr) s += ' ' + devinfo.hrstr;
		}
		s = "Device"+s+" info # hw: 0x" + hex(cfg.hw_version,4)
		   + ", sw: 0x" + hex(cfg.sw_version,4)
		   + ", services: 0x" + hex(cfg.services,8)
		   + ", sd: 0x" + hex(cfg.dev_spec_data, 4);
		let sid1 = cfg.dev_spec_data & 0x0ff;
		let sid2 = (cfg.dev_spec_data & 0x0ff00) >> 8;
		if(sid1 < id_sensor_str.length && sid2 < id_sensor_str.length)
			s += " (Sensor1: " + id_sensor_str[sid1] +", Sensor2: " + id_sensor_str[sid2] + ")";
		addLog(s);
		if(bthomeEnabled) 
			cfg.zbdevice = ((cfg.services & 0x01000000) != 0); // SERVICE_ZIGBEE
		if(cfg.zbdevice) {  
			fwmaxsize = MAX_EXT_OTA_SIZE;
			cfg.ver = cfg.sw_version & 0xff;
			s = "ZBdevice: " + hw_version_str[cfg.hw_version]+', Software Version: '+(cfg.ver>>4)+'.'+(cfg.ver&0x0f);
			setStatus(s);
			menuUpgrade();
		}
	} else if(blkid == 0x33 && len >= 8) {
		let vbat = value.getUint16(1, true);
		let s = '';
		if(len == 17) {
			let i1 = value.getInt16(3, true);
			let i2 = value.getInt16(5, true);
			let i3 = value.getInt16(7, true);
			let u1 = value.getUint16(9, true);
			let u2 = value.getUint16(11, true);
			let u3 = value.getUint16(13, true);
			let count = value.getUint16(15, true);
			let flg = value.getUint8(17);
			s = 'Vbat: '+vbat+' mV, I: '+i1+':'+i2+':'+i3+' mA, U: '+u1+':'+u2+':'+u3+' mV, ID: '+count+', TRG Out: ' + ((flg & 2) >> 1) + ', flg: 0x'+hex(flg,2);
		} else {
			let temp = value.getInt16(3, true) / 100.0;
			let humi = value.getUint16(5, true) / 100.0;
			let count = value.getUint16(7, true);
			if(hwver_id == 53 && len > 9) {
				let co2 = value.getUint16(9, true);
				s = 'CO<sub>2</sub>: '+co2+' ppm, Vbat: '+vbat+' mV, Temp: '+temp.toFixed(2)+'°C, Humi: '+humi.toFixed(2)+'%, ID: '+count;
			} else {
			let flg = 0;
			let rds_count = 0;
			if(len > 8) {
				flg = value.getUint8(9);
				if(len > 12)
					rds_count = value.getUint32(10, true);
			}
			let sw = 'Open';
			if((flg & 1) == 0) sw = 'Close';
			if(cfg.ver >= 0x37) {
				s = 'Vbat: '+vbat+' mV, Temp: '+temp.toFixed(2)+'°C, Humi: '+humi.toFixed(2)+'%';
				if(hwver_id == 44)
					s += ', Mois: '+(count/100.0).toFixed(2)+'%';
				else
					s += ', ID: '+count;
				s +=', Switch: '+ sw +', Counter: '+ rds_count+', TRG Out: ' + ((flg & 2) >> 1) + ', flg: 0x'+hex(flg,2);
			} else
				s = 'Vbat: '+vbat+' mV, Temp: '+temp.toFixed(2)+'°C, Humi: '+humi.toFixed(2)+'%, ID: '+count+', Switch ' + sw +', TRG Out: ' + ((flg & 2) >> 1) + ', flg: 0x'+hex(flg,2);
			}
		}
		$("tempHumiData").innerHTML = s;
		addClog(s);
	} else if(blkid == 0x21 && len >= 10) {
			trsc.int = value.getUint32(1, true);
			trsc.mac = new Uint8Array(value.buffer.slice(5,11));
			trsc.mact = hex(trsc.mac[5],2)+hex(trsc.mac[4],2)+hex(trsc.mac[3],2)+hex(trsc.mac[2],2)+hex(trsc.mac[1],2)+hex(trsc.mac[0],2);
			s = 'TrSc Config: interval: '+trsc.int+', MAC: '+trsc.mact;
			addAlog(s);
			if(cfg.ver >= 0x53 && ((hwver_id == 0)||(hwver_id == 3)||(hwver_id == 4)||(hwver_id == 5)||(hwver_id == 10)||(hwver_id == 14))) {
				$('trsc_int').value = trsc.int;
				$('trsc_mac').value = trsc.mact;
			};
	} else if(blkid == 0x22 && len >= 7) {
		if((hwver_id == 9) || (hwver_id == 12) || (hwver_id == 49)) {
			ext.big_number = value.getInt32(1, true); // -9950..199950, x0.01
			ext.small_number = 0;
		} else {
			ext.big_number = value.getInt16(1, true); // -995..19995, x0.1
			ext.small_number = value.getInt16(3, true); // -9..99, x1
		}
		ext.vtime = value.getUint16(5, true); // in sec
		ext.flg = value.getUint8(7);
		s = 'ExtShow: big_number: '+ext.big_number+', small_number: '+ext.small_number+', ext.vtime: '+ext.vtime+' s, flg: '+hex(ext.flg,2);
		addAlog(s); UpdExt();
	} else if((blkid == 0x25 || blkid == 0x26 || blkid == 0x2b || blkid == 0x2c)  && (len > 16)) {
	// CMD_ID_CFS Get/Set sensor config
	    devSens.temp_k = value.getUint32(1, true);
	    devSens.humi_k = value.getUint32(5, true);
	    devSens.temp_z = value.getInt16(9, true);
	    devSens.humi_z = value.getInt16(11, true);

		$('inpTempK').value  = (devSens.temp_k/100.0).toFixed(2);
		$('inpHumK').value = (devSens.humi_k/100.0).toFixed(2);
		$('inpTempZ').value = (devSens.temp_z/100.0).toFixed(2);
		$('inpHumZ').value  = (devSens.humi_z/100.0).toFixed(2);
		devSens.i2c_addr = value.getUint8(17);
		if(len > 17) { // ver 4.9
			devSens.id = value.getUint32(13, true);
			devSens.sentype = value.getUint8(18);
			if(devSens.sentype < id_sensor_str.length) {
				st = id_sensor_str[devSens.sentype];
				if (typeof addMenuSensors === 'function') addMenuSensors();
			} else
			    st = "N:" + devSens.sentype;
		} else {
			devSens.id = value.getUint32(13, false);
			if((devSens.id & 0xFFFF) == 0x2000)
				st = "AHT20/30";
			else if((devSens.id & 0xFFFF) == 0x3000)
				st = "SHT30";
			else if(devSens.id == 0x83055959)
				st = "CHT8305";
			else if(devSens.i2c_addr == 0xE0)
				st = "SHTv3";
			else if(devSens.i2c_addr == 0x88 || devSens.i2c_addr == 0x8A)
				st = "SHT4x";
		}
		let s = "<b>Sensor Settings:</b> (" + st;
		if(devSens.i2c_addr != 0)
			s += ", I2C address: 0x"+hex(devSens.i2c_addr,2)
		s += ", ID: " + hex(devSens.id, 8) + ")";
		if(blkid == 0x2b || blkid == 0x2c)
			 s += ", part: " + (3 - (blkid & 1));
		$('lblSensor').innerHTML = s;
		addAlog("Sensor "+st+" id: "+hex(devSens.id, 8)
		   + ", I2C address: 0x" + hex(devSens.i2c_addr,2)
		   + ", Kt: " + devSens.temp_k
		   + ", Kh: " + devSens.humi_k
		   + ", Zt: " + devSens.temp_z
		   + ", Zh: " + devSens.humi_z);

	} else if((blkid >= 0x27 && blkid <= 0x28) && (len > 6)) {
	// CMD_ID_CFB20 Get/Set sensor MY18B20 config
	    devSens.temp2_k = value.getUint32(1, true);
	    devSens.temp2_z = value.getInt16(5, true);
		$('inpTemp2K').value  = (devSens.temp2_k/100.0).toFixed(2);
		$('inpTemp2Z').value = (devSens.temp2_z/100.0).toFixed(2);
		addAlog("Sensor2: Kt: " + devSens.temp2_k + ", Zt: " + devSens.temp2_z);
	} else if((blkid == 0x2e || blkid == 0x2f) && (len > 13)) {
	// CMD_ID_SCD Get/Set sensor SCD41 config
		devSens.temp_offset = value.getUint16(1, true);
		devSens.altitude = value.getUint16(3, true);
		devSens.asc_co2 = value.getUint16(5, true);
		devSens.asc_ena = value.getUint16(7, true);
		devSens.asc_ini_per = value.getUint16(9, true);
		devSens.asc_std_per = value.getUint16(11, true);
		devSens.pressure = value.getUint16(13, true);
		$('inpSCD4xTempOffset').value = (devSens.temp_offset*175/65535).toFixed(2);
		$('inpSCD4xAltitude').value = devSens.altitude;
		$('inpSCD4xPressure').value = (devSens.pressure * 100.0).toFixed(0);
		$('inpSCD4xAscCo2').value = devSens.asc_co2;
		$('inpSCD4xInitPeriod').value = devSens.asc_ini_per;
		$('inpSCD4xStdPeriod').value = devSens.asc_std_per;
		$('chbSDC41asc').checked = devSens.asc_ena;
		addAlog("Sensor SCD41: Toff: " + (devSens.temp_offset*175/65535).toFixed(2)
		   + ", Alt: " + devSens.altitude
		   + ", Prs: " + (devSens.pressure * 100.0).toFixed(0)
		   + "; ASC: enable: " + devSens.asc_ena
		   + ", CO2: " + devSens.asc_co2
		   + ", It: " + devSens.asc_ini_per
		   + ", Is: " + devSens.asc_std_per);
	} else if(blkid == 0x77 && len > 1) {
		devSens.cmd = value.getUint8(1);
		let s = "";
		if(len > 4) {
			devSens.errors = value.getUint16(2, true);
			devSens.value = value.getUint16(4, true);
			addAlog("Sensor SCD41 ExtStatus: cmd: " + devSens.cmd
			   + ", value: " + devSens.value
			   + "; errors: " + devSens.errors);
			s ='Response: ok, Command: '+devSens.cmd+', Status: ' + hex(devSens.value,4);
			if(devSens.errors == 0) {
				if(devSens.cmd == 5 || devSens.cmd == 6) getSensSCD4x();
				else if(devSens.cmd == 2) {
					if(devSens.value == 0xffff) s='Recalibration Failed: Status: ' + hex(devSens.value,4);
					else s='Response: ok, Command: '+devSens.cmd+', FCR-correction: ' + (devSens.value-0x800);	
				}
			} else	s='Error: '+hex(devSens.errors,4)+', Command: '+devSens.cmd+', Status: ' + hex(devSens.value,4);
		} else {
			let err = value.getUint8(1);
			if(err == 0xff) s ='Command number error!';
			else if(err == 0xfe) s ='Please wait: previous command is being executed!';
			else s ='Unknown error!';
		}
		$('txtSCD4xResponse').innerHTML = '<b>' + s + '</b>';
	} else if(blkid == 0x44 && len >= 7) {
		trg.tmp_thr = value.getInt16(1, true) / 100.0; // temp threshold
		trg.hm_thr = value.getInt16(3, true) / 100.0; // humi threshold
		trg.rds_type = 0;
		trg.rds_rpint = 0;
		if(len >= 9) {
			trg.tmp_hst = value.getInt16(5, true) / 100.0;		  // temp hysteresis
			trg.hm_hst = value.getInt16(7, true) / 100.0;		 // humi hysteresis
			if((len >= 12) && (cfg.ver >= 0x37)) {
				trg.rds_rpint = value.getUint16(9, true);
				trg.rds_type = value.getUint8(11);
				trg.flg = value.getUint8(12);
			} else
				trg.flg = value.getUint8(9);
		} else {
			trg.tmp_hst = value.getInt8(5) / 10.0;		  // temp hysteresis
			trg.hm_hst = value.getInt8(6) / 10.0;		 // humi hysteresis
			trg.flg = value.getInt8(7);
		}
		let s = '';
		if(cfg.ver >= 0x37)
			s = 'Threshold Temp/Humi: '+trg.tmp_thr.toFixed(2)+'°C/'+trg.hm_thr.toFixed(2)+'%, Hysteresis T/H: '+trg.tmp_hst.toFixed(2)+'°/'+trg.hm_hst.toFixed(2)+'%, Reed switch mode: '+trg.rds_type+', Rs rep.interval: '+trg.rds_rpint+' sec, flg: 0x'+hex(trg.flg,2);
		else
			s = 'Threshold Temp/Humi: '+trg.tmp_thr.toFixed(2)+'°C/'+trg.hm_thr.toFixed(2)+'%, Hysteresis T/H: '+trg.tmp_hst.toFixed(2)+'°/'+trg.hm_hst.toFixed(2)+'%, flg: 0x'+hex(trg.flg,2);
		if((cfg.ver >= 0x39)&&((trg.rds_type&0x10)!=0)) s += ', RS input inversion'
		addAlog(s); UpdTrg();
	} else if(blkid >= 0x10 && blkid <= 0x14 && len >= 1) {
		let lb = value.getUint8(1);
		// addClog('id: '+blkid+', lb: '+lb+ ', len: ' + len);
		if(lb != 0) {
			len -= 1;
			if(cnt_buf11 == 0){
				//addClog('New miKey length: '+lb);
				buf11 = new Uint8Array(lb);
			}
			for(let i = 0; i < len && i < lb; i++ )
				buf11[cnt_buf11++] = value.getUint8(i+2);
			if(len == lb) {
				if(cnt_buf11 == buf11.length) {
					s = '';
					if(blkid == 0x14)  {
						if(cnt_delkeys == 0) {
							mikeys.delkeys = [];
						}
						s = 'Marked as delete Key'+cnt_delkeys+': ';
						cnt_delkeys += 1;
						mikeys.delkeys.push(buf11);
					}
					if(cnt_buf11 == 28) {
						s +=  'miToken: '+dump(buf11.slice(0, 12), 12)+', miBindKey: '+dump(buf11.slice(12), 16);
						if(blkid == 0x12) {
							cnt_delkeys = 0;
							mikeys.token = buf11.slice(0, 12);
							mikeys.bindkey = buf11.slice(12);
							CustomConfig();
						} else if(blkid == 0x14) {
							if(mikeys.token) mikeys.restore = true;
						}
						addLog(s);
					} else if(cnt_buf11 == 20) {
						let str = new TextDecoder("utf-8").decode(buf11.slice(1));
						s += 'miDevId: "'+str+'" '+dump(buf11.slice(1), 19);
						if(blkid == 0x11) {
							cnt_delkeys = 0;
							mikeys.id = buf11;
						}
						addLog(s);
					} else if(cnt_buf11 == 8) {
						addClog('Custom (0x1f1f) Notifications id: 0x'+hex(blkid,2)+' ['+bytesToHex(value.buffer.slice(1))+']');
						// * public_mac:		[0][1][2][3][4][5], const: [3]=38;[4]=C1;[5]=A4
						// * random_static_mac: [0][1][2][6][7]C0
						m = hex(buf11[5],2)+hex(buf11[4],2)+hex(buf11[3],2)+hex(buf11[2],2)+hex(buf11[1],2)+hex(buf11[0],2);
						s += 'MAC: '+m+', RandMAC: '+'C0'+hex(buf11[7],2)+hex(buf11[6],2)+hex(buf11[2],2)+hex(buf11[1],2)+hex(buf11[0],2);
						if(blkid == 0x10) {
							cnt_delkeys = 0;
							mikeys.mac = buf11;
							addLog(s);
							$("mi_mac").value = m + hex(buf11[7],2)+hex(buf11[6],2);
						}
					} else if(cnt_buf11 == 4) {
						s += 'miCfg: '+dump(buf11, 4);
						if(blkid == 0x13) {
							cnt_delkeys = 0;
							mikeys.cfg = buf11;
							addLog(s);
						}
					} else
						s += 'miKey: '+dump(buf11, cnt_buf11);
					addClog(s);
				}
				cnt_buf11 = 0;
			}
		} else if(cnt_buf11 != 0) {
			cnt_buf11 = 0;
			addClog("Error read mi keys!");
		} else {
			if(blkid == 0x11) addAlog('No miDevId!');
			else if(blkid == 0x12) addAlog('No miToken and miBindKey!');
			else if(blkid == 0x13) addAlog('No miCFG!');
			else if(blkid == 0x14) {addClog("End keys"); CustomConfig();}
			else addClog("End keys");
			cnt_buf11 = 0;
		}
	} else if(blkid == 0x18 && len >= 1) { // Get/set beacon bkey in EEP
		if(len >= 16) {
			mikeys.cbindkey = value.buffer.slice(1);
			let s = bytesToHex(mikeys.cbindkey,16);
			addAlog("Read bindkey: "+ s);
			if($("cbind_key"))
				$("cbind_key").value = s;
		} else {
			if(len == 1 && value.getUint8(1) == 0xff)
				addAlog("No bindkey in EEP!");
			else
				addAlog("Error read bindkey from EEP!");
			if($("cbind_key"))
				$("cbind_key").value = '?';
		}
	} else if(blkid == 0x60 && len >= 6) {
		s = 'LCD data: '+bytesToHex(value.buffer.slice(1));
		addAlog(s);
	} else if(blkid == 0x61 && len >= 1) {
		let s = 'LCD flg: '+hex(value.getUint8(1), 2);
		addClog(s);
	} else if(blkid == 0x35) {
		if(flg_memo_act) {
			if(len >= 12) {
				let cnt = value.getUint16(1, true);
				let tc = value.getUint32(3, true);
				let tm = value.getInt16(7, true) / 100.0;
				let hm = value.getUint16(9, true) / 100.0;
				let vb = value.getUint16(11, true);
				let dt = new Date(tc*1000);
				addAlog(((dt.toISOString().slice(0, -1)).replace('T',' ')).replace('.000','')+' # Vbat: '+vb+' mV, Temp: '+tm+'°C, Humi: '+hm+'%, Count: '+cnt);
			} else if(len >= 2) {
				let cnt = value.getUint16(1, true);
				addAlog('Memo end: '+cnt);
				flg_memo_act = false;
			}
		}
	} else if(blkid == 0x23 && len >= 4) {
		devtime.cur = value.getUint32(1,true);
		addClog('Device Time: 0x' + hex(devtime.cur,8));
		let dt = new Date(devtime.cur*1000);
		addAlog('Device Date: '+(dt.toISOString().slice(0, -1)).replace('T',' '));
		if(len >= 8) {
			devtime.set = value.getUint32(5,true);
			addClog('Last clock setting Time: 0x' + hex(devtime.set,8));
			if(devtime.step == 1) {
				if(devtime.set > 0x60000000) {
					devtime.period = devtime.cur - devtime.set;
					let time = Date.now()/1000;
					time -= (new Date()).getTimezoneOffset() * 60;
					devtime.cmp = time;
					let odt = new Date(devtime.set*1000);
					addAlog('Last clock setting: '+(odt.toISOString().slice(0, -1)).replace('T',' '));
					addAlog('DevPeriod: ' + devtime.period.toFixed(1) + ' sec');
					let rp = devtime.cmp - devtime.set;
					let delta = rp - devtime.period;
					addClog('RealPeriod: ' + rp.toFixed(1) + ' sec, Delta: ' + delta.toFixed(1) + ' sec');
					if(rp >= 10800) { // 10800
						devtime.step = 2;
						addClog("Send cmd Get StepTimeSec...");
						settingsCharacteristics.writeValue(new Uint8Array([0x24])).then(_ => {
							addAlog('Get StepTimeSec...');
						});
					} else {
						devtime.step = 0;
						addAlog('The minimum period for calculation is 3 hours!');
					}

				} else {
					devtime.step = 0;
					addAlog('The clock must be set beforehand!');
				}
			}
		}
	} else if(blkid == 0x24 && len >= 4) {
		cfg.step_time = value.getUint32(1,true);
		addClog('step_time: ' + cfg.step_time);
		addAlog('Device StepTimeSec: ' + (cfg.step_time / 16.0).toFixed(3) + ' us');
		if($("cfg_time_step") && cfg.ver >= 0x24) {
			if(devtime.step == 2) {
						devtime.step = 0;
				let rp = devtime.cmp - devtime.set;
				let k = devtime.period/rp;
				addClog('koef: ' + k);
				let nstep_time = cfg.step_time * k;
				$("cfg_time_step").value = nstep_time - 16000000;
				addClog('step_time: ' + nstep_time.toFixed(1));
			} else
				$("cfg_time_step").value = cfg.step_time - 16000000;
		}
	} else if(blkid == 0x20 && len >= 8) {
		cmf.tmp_lo = value.getInt16(1, true); // temp lo
		cmf.tmp_hi = value.getInt16(3, true); // temp hi
		cmf.hm_lo = value.getUint16(5, true); // humi lo
		cmf.hm_hi = value.getUint16(7, true); // humi lo
		let s = 'Comfort Temp: '+(cmf.tmp_lo/100.0).toFixed(2)+'..'+(cmf.tmp_hi/100.0).toFixed(2)+'°C, Humi: '+(cmf.hm_lo/100.0).toFixed(2)+'..'+(cmf.hm_hi/100.0).toFixed(2)+'%';
		addAlog(s); UpdCmf();
	} else if(blkid == 0x73 && len >= 9) {
		let e = value.getUint8(1);
		let faddr = value.getUint32(2, true);
		let fszk = value.getUint32(6, true); // in kbytes
		let s = 'OTA region: 0x'+hex(faddr,6)+', size: '+fszk+' kbytes';
		if(e == 0) s = 'Information: Current '+s;
		else if(e == 1) s += ' -> OTA works';
		else if(e == 2) s += ' -> Ext.OTA busy';
		else if(e == 3) s += ' -> Ext.OTA ready';
		else if(e == 4) s += ' -> Ext.OTA event';
		else if(e > 0x80) s += ' -> Ext.OTA error!';
		else e += ' ('+e+')';
		addClog(s);
		if(bigOtaEnabled) {
			if(e == 4) {
				s = 'Erase the Flash sector at: 0x'+hex(faddr,6)+'...';
				setStatus(s);
				addClog(s);
			} else if(e == 3) {
				s = 'Clean Flash OTA region: 0x'+hex(faddr,6)+', size: '+fszk+' kbytes. Go OTA...';
				setStatus(s);
				addLog(s);
				fwmaxsize = (fszk << 10) + 4096;
				setTimeout(updateBegin(), 1000);
			} else if(e == 2) {
				s = "Waiting Start ext.OTA...";
				setStatus(s);
				addLog(s);
			} else {
				addLog(s);
			}
		} else {
			s = "Error! Ext.OTA is not supported!";
			setStatus(s);
			addClog(s);
		}
	} else if(blkid == 0x01 && len >= 1) {
		dnm.name = new TextDecoder("utf-8").decode(value.buffer.slice(1));
		if($("dev_name"))
			$("dev_name").value = dnm.name;
		addAlog("DevName: ["+dnm.name+"]");
	} else if((blkid == 0x29) && len >= 12) {
	    devSens.rh_k = value.getUint32(1, true);
	    devSens.rh_z = value.getUint16(5, true);
	    devSens.rh_d = value.getUint16(7, true);
		let adc_h = value.getUint16(9, true);
		let adc_d = value.getUint16(11, true);
		addAlog("Config Moisture Sensor - K: "+devSens.rh_k+", Z: "+devSens.rh_z+", D: "+ devSens.rh_d);
		addAlog("ADCh: "+adc_h+" ("+((adc_h*1175)/4096).toFixed(2)+" mV), ADCn: "+adc_d+" ("+((adc_d*1175)/4096).toFixed(2)+" mV)");
	} else if((blkid == 0x2A) && len >= 5) {
		let err = value.getUint8(1);
		let rh = value.getUint16(2, true);
		let ntc = value.getUint16(4, true);
		if(err == 0)
			addAlog("Calibration ok");
		if(err == 1)
			addAlog("Calibration error!");
		if(err == 0xff)
			addAlog("Command error!");
		addAlog("ADCh: "+rh+" ("+((rh*1175)/4096).toFixed(2)+" mV), ADCn: "+ntc+" ("+((ntc*1175)/4096).toFixed(2)+" mV)");
	} else if(blkid == 0x02 && len > 1) {
		let addr = value.getUint8(1);
		s = 'Sensor: ';
		if(addr != 0)
			s += 'I2C addres 0x'+hex(addr >> 1,2);
		else
			s += 'None!';
		s += ', LCD driver: ';
		addr = value.getUint8(2);
		if(addr != 0)
			s += 'I2C addres 0x'+hex(addr >> 1,2);
		else
			s += 'SPI or UART';
		if(len > 2) {
			addr = value.getUint8(3);
			s += ', RTC: ';
			if(addr != 0)
				s += 'I2C addres 0x'+hex(addr >> 1,2);
			else
				s += 'Unknown';
		}
		addAlog(s);
	} else if(blkid == 0x04 && len > 0) {
		let wr = value.getUint8(1);
		if(len == 1)
				s = 'I2C Read/Write fault! ('+wr+')';
		else {
			let addr = value.getUint8(2);
			s = 'I2C addres 0x'+hex(addr,2);
			if(wr != 0)
				s += ', write '+wr+' bytes';
			if(len > 2)
				s += ', read '+ (len-2) +' bytes: '+bytesToHex(value.buffer.slice(3));
		}
		addAlog(s);
	} else if(blkid == 0x05 && len > 3) {
		addAlog('Sensor ID: ' + hex(value.getUint32(1,true),8));
	} else if(blkid == 0x45) {
		let flg = value.getUint8(1);
		let s = 'Switch1: Close';
		if((flg & 1) == 0)
			s = 'Switch1: Open';
		let	sw = 'Close';
		if((flg & 0x40) == 0) sw = 'Open';
		s += ', Switch2: ' + sw +', TRG Out "' + ((flg & 2) >> 1) + '", flg: 0x'+hex(flg,2);
		$("tempHumiData").innerHTML = s;
		addClog(s);
	} else if(blkid == 0x03) {
		addAlog('Custom (0x1f1f) Notifications id: 0x'+hex(blkid,2)+' ['+bytesToHex(value.buffer.slice(1))+']');
	} else {
		addClog('Custom (0x1f1f) Notifications id: 0x'+hex(blkid,2)+' ['+bytesToHex(value.buffer.slice(1))+']');
	}
}

function decimalToHex00(d) {
    var hex = Number(d).toString(16);
    while (hex.length < 2) {
        hex = "0" + hex;
    }
    return hex;
}

const lcd_digcode = [0xf5,0x05,0xd3,0x97,0x27,0xb6,0xf6,0x15,0xf7,0xb7];
function lcd_clock() {
	let date = new Date();
	let hours = date.getHours();
	let minutes = date.getMinutes();
	let lcdb = new Uint8Array(7);
	lcdb[0] = 0x60;
	lcdb[1] = lcd_digcode[parseInt(minutes % 10)];
	lcdb[2] = lcd_digcode[parseInt(minutes / 10)];
	lcdb[4] = lcd_digcode[parseInt(hours % 10)];
	lcdb[5] = lcd_digcode[parseInt(hours / 10)];
	addClog("Send cmd (60): Set LCD data");
	settingsCharacteristics.writeValue(lcdb).then(_ => {addClog('Send data ok'); addLog("Send 'Send clock data on LCD' ok");});
}
function setDevTime() {
	let time = Date.now()/1000;
	time -= (new Date()).getTimezoneOffset() * 60;
	blk = new Uint8Array(5);
	blk[0] = 0x23;
	blk[1] = time & 0xff;
	blk[2] = (time >> 8) & 0xff;
	blk[3] = (time >> 16) & 0xff;
	blk[4] = (time >> 24) & 0xff;
	addClog("Send cmd Set DevTime ("+dump(blk, blk.length)+")...");
	settingsCharacteristics.writeValue(blk).then(_ => {
		addAlog('Send new DevTime ok');
	});
}
function calkDeltaTime() {
	devtime.step = 1;
	addClog("Send cmd Get DevTime...");
	blk = new Uint8Array(1);
	blk[0] = 0x23;
	settingsCharacteristics.writeValue(blk).then(_ => {
		addAlog('Get DevTime...');
	});
}
function lcd_restore() {
	addClog("Send cmd (6100): Restore show LCD");
	settingsCharacteristics.writeValue(new Uint8Array([0x61, 0])).then(_ => {addClog('Send data ok'); addLog("Send 'Repair LCD' ok");});
}

function sendPinCode() {
	if(pincode.enable) {
		let el = $("pincode");
		let x = parseInt(el.value);
		let s = ("0000000" + x.toString(10)).slice(-6);
		if(el.value.length == 6 && x <= 999999 && x >= 0) {
			el.value = s;
			addLog("PinCode: '"+s+"'");
			settingsCharacteristics.writeValue(new Uint8Array([0x70, x&0xff, (x>>8)&0xff, (x>>16)&0xff, (x>>24)&0xff])).then(_ => {addAlog('Send pincode ok');});
		} else	{
			el.value = s;
			addLog("Must be 6 decimal digits! 000000..999999, 6 digits, if '000000' - PinCode Disable.")
		}
	}
}
function CleanDevName() {
	addClog("Send cmd (0100) Clean DevName...");
	settingsCharacteristics.writeValue(new Uint8Array([0x01, 0])).then(_ => {
			addAlog('Send Clean DevName ok');
	});
}
function sendDevName() {
	if(dnm.enable) {
		let el = $("dev_name").value;
		if(el.length >= 1 && el.length <= 18) {
			let encoder = new TextEncoder();
			blk = new Uint8Array(el.length + 1);
			blk.set(encoder.encode(el), 1);
			blk[0] = 0x01;
			addClog("Send cmd New DevName ("+dump(blk, blk.length)+")...");
			settingsCharacteristics.writeValue(blk).then(_ => {
				addAlog('Send New DevName ok');
			});
		}
	}
}
function CleanMAC() {
	addClog("Send cmd Clean MAC (1000)...");
	settingsCharacteristics.writeValue(new Uint8Array([0x10, 0])).then(_ => {
		addAlog('Send Clean MAC ok');
	});
}
function sendMAC() {
	if(mikeys.mac) {
		let el = $("mi_mac").value;
		let len = el.length;
		addClog(len + ' ' + el);
		if(len == 12 || len == 16) {
			let mac = hexToBytes(el);
			len = mac.length;
			addClog(len + ' ' + el);
			if(len == 6 || len == 8) {
				let blk = new Uint8Array(len+2);
				blk[0] = 0x10;
				blk[1] = len;
				blk[2] = mac[5];
				blk[3] = mac[4];
				blk[4] = mac[3];
				blk[5] = mac[2];
				blk[6] = mac[1];
				blk[7] = mac[0];
				if(len == 8) {
					blk[8] = mac[7];
					blk[9] = mac[6];
				}
				addClog(blk);
				addClog("Send cmd New MAC ("+dump(blk, blk.length)+")...");
				settingsCharacteristics.writeValue(blk).then(_ => {
					s = "Send New MAC: "+dump(mac, 6);
					if(len == 8)
						s += " RAND:" +dump(mac.slice(6), 2);
					addAlog(s+" ok");
				});
			}
		} else
			addLog("Must be 6 hex MAC digits [+ 2 hex RandMAC digits]!")
		return;
	}
}

function UpdExt() {
	if(ext.enable && $("extbignumb").value) {
		if((hwver_id == 9) || (hwver_id == 12) || (hwver_id == 49)) {
			$("extbignumb").value = (ext.big_number / 100.0).toFixed(2);
			$("exttmpsmb").value = (ext.flg >> 4) & 0x07;
			$("extpersent").checked = ((ext.flg & 0x80) !=0 ? 1 : 0);
			$("extbattery").checked = ((ext.flg & 8) !=0 ? 1 : 0);
		} else {
			$("extbignumb").value = (ext.big_number / 10.0).toFixed(1);
			if(hwver_id == 2 || hwver_id == 6 || hwver_id == 7)	 // CGG1 & CGDK2
				$("extsmalnumb").value = (ext.small_number / 10.0).toFixed(1);
			else
				$("extsmalnumb").value = ext.small_number.toFixed(0);
			$("extpersent").checked = ((ext.flg & 8) !=0 ? 1 : 0);
			$("exttmpsmb").value = (ext.flg >> 5) & 0x07;
			$("extbattery").checked = ((ext.flg & 16) !=0 ? 1 : 0);
		}
		$("extsmiley").value = ext.flg & 7;
		$("extvtimed").value = ext.vtime;
	}
}

function sendExt() {
	if(ext.enable) {
		if((hwver_id == 9) || (hwver_id == 12) || (hwver_id == 49)) {
			ext.big_number = Math.round(100.0 * parseFloat($("extbignumb").value));
			ext.cfg = (parseInt($("extsmiley").value) & 7) +
			($("extbattery").checked ? 8 : 0) +
			($("extpersent").checked ? 0x80 : 0) +
			((parseInt($("exttmpsmb").value) & 0x07) << 4);
		} else {
			ext.big_number = Math.round(10.0 * parseFloat($("extbignumb").value));
			if(hwver_id == 2 || hwver_id == 6 || hwver_id == 7) // CGG1 & CGDK2
				ext.small_number = Math.round(10.0 * parseFloat($("extsmalnumb").value));
			else
				ext.small_number = parseInt($("extsmalnumb").value);
			ext.cfg = (parseInt($("extsmiley").value) & 7) +
			($("extpersent").checked ? 8 : 0) +
			($("extbattery").checked ? 16 : 0) +
			((parseInt($("exttmpsmb").value) & 7) << 5);
		}
		ext.vtime = parseInt($("extvtimed").value);
		if(ext.vtime < 2) ext.vtime = 2;
		else if(ext.vtime > 65535) ext.vtime = 65535;
		addClog("Send cmd (22 + ext data)...");
		let blk;
		if((hwver_id == 9) || (hwver_id == 12) || (hwver_id == 49)) {
			blk = new Uint8Array([0x22, ext.big_number&0xff, (ext.big_number>>8)&0xff, (ext.big_number>>16)&0xff, (ext.big_number>>24)&0xff, ext.vtime&0xff,(ext.vtime>>8)&0xff,ext.cfg]);
		} else{
			blk = new Uint8Array([0x22, ext.big_number&0xff, (ext.big_number>>8)&0xff,ext.small_number&0xff,(ext.small_number>>8)&0xff,ext.vtime&0xff,(ext.vtime>>8)&0xff,ext.cfg]);
		}
		settingsCharacteristics.writeValue(blk).then(_ => {
			addAlog('Send Ext data: '+dump(blk, blk.length)+' ok');
		});
	}
}
function UpdCmf() {
	if(cmf.enable && $("cmf_tmp_lo").value) {
		$("cmf_tmp_lo").value =	 (cmf.tmp_lo/100.0).toFixed(2);
		$("cmf_tmp_hi").value =	 (cmf.tmp_hi/100.0).toFixed(2);
		$("cmf_hm_lo").value =	(cmf.hm_lo/100.0).toFixed(2);
		$("cmf_hm_hi").value =	(cmf.hm_hi/100.0).toFixed(2);
	}
}
function SendCmf() {
	if(cmf.enable) {
		cmf.tmp_lo = Math.round(100.0 * parseFloat($("cmf_tmp_lo").value));
		cmf.tmp_hi = Math.round(100.0 * parseFloat($("cmf_tmp_hi").value));
		cmf.hm_lo = Math.round(100.0 * parseFloat($("cmf_hm_lo").value));
		cmf.hm_hi = Math.round(100.0 * parseFloat($("cmf_hm_hi").value));
		addClog("Send cmd (20 + cmf data)...");
		settingsCharacteristics.writeValue(new Uint8Array([0x20, cmf.tmp_lo&0xff, (cmf.tmp_lo>>8)&0xff, cmf.tmp_hi&0xff, (cmf.tmp_hi>>8)&0xff, cmf.hm_lo&0xff, (cmf.hm_lo>>8)&0xff, cmf.hm_hi&0xff, (cmf.hm_hi>>8)&0xff])).then(_ => {
			addAlog('Send Cmf data ok');
		});
	}
}
function UpdTrg() {
	if(trg.enable && $("trg_tmp_hst").value) {
		$("trg_tmp_thr").value =  trg.tmp_thr.toFixed(2);
		$("trg_hm_thr").value =	 trg.hm_thr.toFixed(2);
		$("trg_tmp_hst").value =  trg.tmp_hst.toFixed(2);
		$("trg_hm_hst").value =	 trg.hm_hst.toFixed(2);
		if(cfg.ver >= 0x37) {
			$("rds_rpint").value =	trg.rds_rpint;
			$("rds_type").value =  trg.rds_type & 3;
			if(trg.rds2_enable) {
				$("rds2_type").value =  (trg.rds_type >> 2) & 3;
			}
		}
		if(cfg.ver >= 0x39) {
			$("rds_invert").checked = ((trg.rds_type & 0x10) != 0);
			if(trg.rds2_enable) {
				$("rds2_invert").checked = ((trg.rds_type & 0x20) != 0);
			}
		}
	}
}
function setNewTBKey() {
	let bk = $("mi_bind_key").value;
	if(bk.length == 32) {
		let bkey = hexToBytes(bk);
		if(bkey.length == 16) {
			let tk = $("mi_token").value;
			if(tk.length == 24) {
				let token = hexToBytes(tk);
				if(token.length == 12) {
					let blk = new Uint8Array(29);
					blk.set(token,1);
					blk.set(bkey,13);
					blk[0] = 0x12;
					addClog(blk);
					addClog("Send cmd MtuSizeExchange (7120)");
					settingsCharacteristics.writeValue(new Uint8Array([0x71,0x20])).then(_ => {
						addClog("Send cmd New bindkey ("+dump(blk, blk.length)+")...");
						settingsCharacteristics.writeValue(blk).then(_ => {
							addAlog("Send cmd New keys ok");
					})});
					return;
				}
			}
			addLog("Mi Token must be 24 hex characters (12 bytes)!")
			return;
		}
	}
	addLog("Bind Key must be 32 hex characters (16 bytes)!")
}
function sendTrg() {
	if(trg.enable) {
		let blk;
		trg.tmp_thr = Math.round(100.0 * parseFloat($("trg_tmp_thr").value));
		trg.hm_thr = Math.round(100.0 * parseFloat($("trg_hm_thr").value));
		if(cfg.ver >= 0x26) {
			trg.tmp_hst = Math.round(100.0 * parseFloat($("trg_tmp_hst").value));
			trg.hm_hst = Math.round(100.0 * parseFloat($("trg_hm_hst").value));
			addClog("Send cmd (44 + trg data)...");
			if(cfg.ver >= 0x37) {
				trg.rds_rpint = parseInt($("rds_rpint").value);
				trg.rds_type = $("rds_type").value & 3;
				if(trg.rds2_enable)
					trg.rds_type |= ($("rds2_type").value & 3) << 2;
				if(cfg.ver >= 0x39) {
					if(trg.rds_type == 3) trg.rds_type += 0x10;
					else trg.rds_type += (($("rds_invert").checked)? 0x10 : 0);
					if(trg.rds2_enable)
						trg.rds_type += (($("rds2_invert").checked)? 0x20 : 0)
				}
				blk = new Uint8Array([0x44, trg.tmp_thr&0xff, (trg.tmp_thr>>8)&0xff,trg.hm_thr&0xff,(trg.hm_thr>>8)&0xff,trg.tmp_hst&0xff,(trg.tmp_hst>>8)&0xff,trg.hm_hst&0xff,(trg.hm_hst>>8)&0xff,trg.rds_rpint&0xff,(trg.rds_rpint>>8)&0xff,trg.rds_type]);
			} else {
				blk = new Uint8Array([0x44, trg.tmp_thr&0xff, (trg.tmp_thr>>8)&0xff,trg.hm_thr&0xff,(trg.hm_thr>>8)&0xff,trg.tmp_hst&0xff,(trg.tmp_hst>>8)&0xff,trg.hm_hst&0xff,(trg.hm_hst>>8)&0xff]);
			}
		} else {
			trg.tmp_hst = Math.round(10.0 * parseFloat($("trg_tmp_hst").value));
			trg.hm_hst = Math.round(10.0 * parseFloat($("trg_hm_hst").value));
			addClog("Send cmd (44 + trg data)...");
			blk = new Uint8Array([0x44, trg.tmp_thr&0xff, (trg.tmp_thr>>8)&0xff,trg.hm_thr&0xff,(trg.hm_thr>>8)&0xff,trg.tmp_hst&0xff,trg.hm_hst&0xff]);
		}
		settingsCharacteristics.writeValue(blk).then(_ => {	addAlog('Send Trg data: '+dump(blk, blk.length)+' -ok');});
	}
}
function sendDeltaTime() {
	let dtim = parseInt($("cfg_time_step").value);
	if(dtim > -32768 && dtim < 32768) {
		addClog("Send cmd set delta (24 + delta: '+dtim+' )...");
		settingsCharacteristics.writeValue(new Uint8Array([0x24, dtim&0xff, (dtim>>8)&0xff])).then(_ => {
			addAlog('Send delta time ok');
			setDevTime();
		});
	} else {
		addAlog("Delta time -32767 to 32767!");
	}
}
function sendGetMemo(cnt) {
	let count = cnt & 0x7fff;
	let start = 0;
	if(count > 19632) count = 19632;
	if(start < 0) start = 0;
	else if(start >= count) start = count - 1;
	blk = new Uint8Array([0x35, count&0xff, (count>>8)&0xff,start&0xff,(start>>8)&0xff]);
	addClog("Send cmd GetMemo: ("+dump(blk, blk.length)+")...");
	settingsCharacteristics.writeValue(blk).then(_ => {
		flg_memo_act = true;
		addAlog('Send cmd GetMemo ok');
	});
}
function getTrScConfig() {
		settingsCharacteristics.writeValue(new Uint8Array([0x21])).then(_ => {
			addAlog('Get TrSc parameters...');
		});
}
function setTrScConfig() {
	if(settingsCharacteristics != null) {
		let el = $("trsc_mac").value;
		let interval = parseInt($("trsc_int").value);
		let len = el.length;
		if(len == 12) {
			let mac = hexToBytes(el);
			len = mac.length;
			if(len == 6) {
				blk = new Uint8Array(11);
				blk[0] = 0x21; 
				blk[1] = interval&0xff; 
				blk[2] = (interval>>8)&0xff; // интервал в сек, =0 - отключено
				blk[3] = (interval>>16)&0xff; 
				blk[4] = (interval>>24)&0xff; 
				blk[5] = mac[5];
				blk[6] = mac[4];
				blk[7] = mac[3];
				blk[8] = mac[2];
				blk[9] = mac[1];
				blk[10] = mac[0];
				addClog("Send TrSc parameters ("+dump(blk, blk.length)+")...");
				settingsCharacteristics.writeValue(blk).then(_ => {
					s = "Send TrSc parameters: interval: "+interval+" sec,  MAC: "+dump(mac, 6);
					addAlog(s+" ok");
				});
			}
		} else
			addLog("Must be 6 hex MAC digits!");
		return;
	}
}
function setSensCfg() {
	if(settingsCharacteristics != null) {
		devSens.temp_k = Math.round(100.0 * parseFloat($('inpTempK').value));
		devSens.humi_k = Math.round(100.0 * parseFloat($('inpHumK').value)); 
		devSens.temp_z = Math.round(100.0 * parseFloat($('inpTempZ').value));
		devSens.humi_z = Math.round(100.0 * parseFloat($('inpHumZ').value));

		blk = new Uint8Array([0x25, 
		    devSens.temp_k & 0xff, (devSens.temp_k >> 8) & 0xff, (devSens.temp_k >> 16) & 0xff, (devSens.temp_k >> 24) & 0xff,
		    devSens.humi_k & 0xff, (devSens.humi_k >> 8) & 0xff, (devSens.humi_k >> 16) & 0xff, (devSens.humi_k >> 24) & 0xff,
	    	devSens.temp_z & 0xff, (devSens.temp_z >> 8) & 0xff,
		    devSens.humi_z & 0xff, (devSens.humi_z >> 8) & 0xff
		    ]);
		settingsCharacteristics.writeValue(new Uint8Array(blk)).then(_ => {	addAlog("Send Sensor Settings: "+dump(blk, blk.length)+" - ok")})
		.catch(error => { addAlog("Send Sensor Settings Error: " + error); });
	}
}
function addMenuSensors() {
	if(devSens.sentype && devSens.sentype == 14) {
		let s = '<hr><label id="lblSensorSCD41"><b>SCD41 Settings:</b></label><br>Temp. Offset: <input size="8" type="text" id="inpSCD4xTempOffset" maxlength="16"> °C, Altitude: <input size="8" type="text" id="inpSCD4xAltitude" maxlength="16"> m, Pressure: <input size="8" type="text" id="inpSCD4xPressure" maxlength="16"> Pa<br><br> <label><b>Automatic Self Calibration:</b><br></label> Enabled: <input type="checkbox" id="chbSDC41asc">, ASC CO2: <input size="8" type="text" id="inpSCD4xAscCo2" maxlength="16"> ppm<br>Initial Period: <input size="8" type="text" id="inpSCD4xInitPeriod" maxlength="16"> , Standard Period: <input size="8" type="text" id="inpSCD4xStdPeriod" maxlength="16"><br><button type="button" id="btnGetSensSCD4x" onclick="getSensSCD4x()">Get Sensor Settings</button> <button type="button" id="btnSetSensSCD4x" onclick="setSensSCD4x()">Send Sensor Settings</button>	<button type="button" id="btnSetDefCfgSCD4x" onclick="setDefCfgSCD4x()">Set Default</button><br><label><b>Chip Functions:</b></label><br> <button type="button" id="btnf3SensSCD4x" onclick="fnSensSCD4x(2)" >Forced Recalibration:</button> <input size="8" type="text" id="inpSCD4xPFRecal" maxlength="16" value="400"> ppm<br><button type="button" id="btnf4SensSCD4x" title="The Read Current Settings command reading current settings from SCD41." onclick="fnSensSCD4x(3)" >Read Current Settings</button> <button type="button" id="btnf6SensSCD4x" title="The Read Settings from EEPROM command reinitializes the sensor by reloading user settings from EEPROM." onclick="fnSensSCD4x(5)" >Read Settings from EEPROM</button> <button type="button" id="btnf5SensSCD4x" title="The Write settings to EEPROM command stores the current configuration in the EEPROM of the SCD4x" onclick="fnSensSCD4x(4)" >Write settings to EEPROM</button><br>	<button type="button" id="btnf8SensSCD4x" title="The perform_self_test command can be used as an end-of-line test to check the sensor functionality" onclick="fnSensSCD4x(7)" >Self Test</button> <button type="button" id="btnf7SensSCD4x" title="The perform_factory_reset command resets all configuration settings stored in the EEPROM and erases the FRC and ASC algorithm history" onclick="fnSensSCD4x(6)" >Factory Reset</button> <label id="txtSCD4xResponse"><b>Response: ?</b></label><br>';
		s += '<label><b>Debug:</b></label> <button type="button" id="btnf1SensSCD4x" onclick="fnSensSCD4x(0)" >Read Run Status</button><br>';
		$("addSensors").innerHTML = s;
		$("addSensors").disabled = false;
		if(devSens.altitude == null)
			getSensSCD4x();
	}
}
function fnSensSCD4x(cmd) {
	if(settingsCharacteristics != null) {
		if(cmd == 0 ) {
			$('txtSCD4xResponse').innerHTML = '<b>Wait response...</b>';
			settingsCharacteristics.writeValue(new Uint8Array(new Uint8Array([0x77]))).then(_ => {	addAlog("Get run status SCD41 - ok")})
			.catch(error => { addAlog("Error get run status SCD41: " + error); });
		} else if(cmd > 0 && cmd < 8){
			let arg = 0;
			if(cmd == 2) {
				arg = parseInt($('inpSCD4xPFRecal').value);
				$('txtSCD4xResponse').innerHTML = '<b>Wait response (2 minutes)...</b>';
			} else $('txtSCD4xResponse').innerHTML = '<b>Wait response...</b>';
			blk = new Uint8Array([0x77,
			    cmd & 0xff,
			    arg & 0xff, (arg >> 8) & 0xff ]);
			settingsCharacteristics.writeValue(new Uint8Array(blk)).then(_ => {	addAlog("Send Command to SCD41: "+dump(blk, blk.length)+" - ok")})
			.catch(error => { addAlog("Send Command to SCD41 Error: " + error); });
		} 
	}
}
function setSensSCD4x() {
	if(settingsCharacteristics != null) {
		devSens.temp_offset = Math.round(parseFloat($('inpSCD4xTempOffset').value)*65535.0/175.0);
		devSens.altitude = parseInt($('inpSCD4xAltitude').value);
		devSens.pressure = parseInt($('inpSCD4xPressure').value) / 100;
		devSens.asc_co2 = parseInt($('inpSCD4xAscCo2').value);
		devSens.asc_ena = (($("chbSDC41asc").checked) ? 1 : 0);
		devSens.asc_ini_per = parseInt($('inpSCD4xInitPeriod').value);
		devSens.asc_std_per = parseInt($('inpSCD4xStdPeriod').value);
		blk = new Uint8Array([0x2e,
		    devSens.temp_offset & 0xff, (devSens.temp_offset >> 8) & 0xff, 
		    devSens.altitude & 0xff, (devSens.altitude >> 8) & 0xff, 
	    	devSens.asc_co2 & 0xff, (devSens.asc_co2 >> 8) & 0xff,
		    devSens.asc_ena & 0x1, 0,
		    devSens.asc_ini_per & 0xff, (devSens.asc_ini_per >> 8) & 0xff,
		    devSens.asc_std_per & 0xff, (devSens.asc_std_per >> 8) & 0xff,
		    devSens.pressure & 0xff, (devSens.pressure >> 8) & 0xff]);
		settingsCharacteristics.writeValue(new Uint8Array(blk)).then(_ => {	addAlog("Send Sensor SCD41 Settings: "+dump(blk, blk.length)+" - ok")})
		.catch(error => { addAlog("Send Sensor SCD41 Settings Error: " + error); });
	}
}
function setSens2Cfg() {
	if(settingsCharacteristics != null) {
		devSens.temp2_k = Math.round(100.0 * parseFloat($('inpTemp2K').value));
		devSens.temp2_z = Math.round(100.0 * parseFloat($('inpTemp2Z').value));
		blk = new Uint8Array([0x27,
		    devSens.temp2_k & 0xff, (devSens.temp2_k >> 8) & 0xff, (devSens.temp2_k >> 16) & 0xff, (devSens.temp2_k >> 24) & 0xff,
	    	devSens.temp2_z & 0xff, (devSens.temp2_z >> 8) & 0xff ]);
		settingsCharacteristics.writeValue(new Uint8Array(blk)).then(_ => {	addAlog("Send Sensor2 Settings: "+dump(blk, blk.length)+" - ok")})
		.catch(error => { addAlog("Send Sensor2 Settings Error: " + error); });
	}
}
function getSensCfg() {
	if(settingsCharacteristics != null) {
		addAlog("Get Sensor Settings...");
		settingsCharacteristics.writeValue(new Uint8Array([0x25])).catch(error => { addAlog("Get Sensor Settings Error: " + error); });
	}
}
function getSensSCD4x() {
	if(settingsCharacteristics != null) {
		addAlog("Get Sensor SCD41 Settings...");
		settingsCharacteristics.writeValue(new Uint8Array([0x2e])).catch(error => { addAlog("Get Sensor SCD41 Settings Error: " + error); });
	}
}
function setDefCfgSCD4x() {
	if(settingsCharacteristics != null) {
		addAlog("Set Default SCD41 Settings...");
		settingsCharacteristics.writeValue(new Uint8Array([0x2f])).catch(error => { addAlog("Set Defasult SCD41 Settings Error: " + error); });
	}
}
function getSens2Cfg() {
	if(settingsCharacteristics != null) {
		addAlog("Get Sensor2 Settings...");
		settingsCharacteristics.writeValue(new Uint8Array([0x27])).catch(error => { addAlog("Get Sensor2 Settings Error: " + error); });
	}
}
function resetSensCfg() {
	if(settingsCharacteristics != null) {
		addAlog("Restore Sensor Default Settings...");
		settingsCharacteristics.writeValue(new Uint8Array([0x26])).catch(error => { addAlog("Restore Sensor Default Settings Error: " + error); });
	}
}
function resetSens2Cfg() {
	if(settingsCharacteristics != null) {
		addAlog("Restore Sensor2 Default Settings...");
		settingsCharacteristics.writeValue(new Uint8Array([0x28])).catch(error => { addAlog("Restore Sensor2 Default Settings Error: " + error); });
	}
}
function getBKey() {
		settingsCharacteristics.writeValue(new Uint8Array([0x18])).then(_ => {
			addAlog('Get binkey from EEP...');
		});
}
function setBKey() {
	let bk = $("cbind_key").value;
	if(bk.length == 32) {
		let bkey = hexToBytes(bk);
		if(bkey.length == 16) {
			let blk = new Uint8Array(17);
			blk.set(bkey,1);
			blk[0] = 0x18;
			addClog("Send bindkey to EEP...");
			settingsCharacteristics.writeValue(blk).then(_ => {
				addAlog("Send new bindkey: " + bytesToHex(blk.slice(1)));
			});
			return;
		}
	}
	addLog("BindKey must be 32 hex characters (16 bytes)!")
}
function InfoIntervals() {
		let advn = parseFloat($("cfg_adv_int").value);
		let meas = parseInt($("cfg_meas_int").value);
		if(advn != 0 && meas != 0) {
			advn = Math.round(advn/62.5) * 62.5;
			$("cfg_meas_int_val").innerHTML = "= " + String((advn * meas / 1000).toFixed(1)) + " sec";
			if(cfg.ver >= 0x20) {
				let memo = parseInt($("cfg_av_meas_mem").value);
				if(memo != 0) {
					let x = advn * meas * memo / 1000;
					if (x < 60)
						$("cfg_av_meas_mem_val").innerHTML = "= " +	 String(x.toFixed(1)) + " sec";
					else
						$("cfg_av_meas_mem_val").innerHTML = "= " +	 String(x.toFixed(1)) + " sec = " + String((x / 60).toFixed(1)) + " min";
				}
			}
		}
}
function CustomConfig() {
 menuUpgrade();
 let is = '';
 if(cfg.ver >= 0x10) {
  is = '<hr>Send commands to custom firmware:<br><input type="text" id="cmdTXT" value=""><button type="button" onclick="sendCustomSetting($(&quot;cmdTXT&quot;).value,settingsCharacteristics);">Send</button>';
  if(hwver_id <= 15 || (hwver_id >= 30 && hwver_id <= 33) || (hwver_id == 38) || (hwver_id == 49) || (hwver_id == 52)) {
	if(hwver_id == 0)
		is += ' Test: [ <button type="button" onclick="lcd_clock();">Show clock on LCD</button> <button type="button" onclick="lcd_restore();">Repair LCD</button> ]<br>';
	is += '<br>Show on device screen:<br>';
	if(hwver_id == 2 || hwver_id == 6 || hwver_id == 7){
		is += '<input size="6" title="Big number: -99.5..1999.5" id="extbignumb" maxlength="5" value="123.4">';
		is += ' <select id="exttmpsmb"><option value="0">&nbsp;&nbsp;</option><option value="1">°Г</option><option value="2">&nbsp;-</option><option value="3">°F</option><option value="4">&nbsp;_</option><option value="5">°C</option><option value="6" selected>&nbsp;=</option><option value="7">°E</option></select>';
		is += ' <select id="extsmiley"><option value="0" selected>&nbsp;&nbsp;&nbsp;</option><option value="5">&nbsp;---&nbsp;</option></select>';
		is += '	<input size="5" title="Small number: -99.5..999.5" id="extsmalnumb" maxlength="5" value="56.7">';
	}else if((hwver_id == 9) || (hwver_id == 12) || (hwver_id == 49)) {
		is += '<input size="8" title="Big number: -999.50..19999.50" id="extbignumb" maxlength="8" value="123.45">';
		is += ' <select id="exttmpsmb"><option value="0">&nbsp;&nbsp;</option><option value="1">°г</option><option value="2">&nbsp;-</option><option value="3">°C</option><option value="4">&nbsp;i</option><option value="5">°Г</option><option value="6" selected>&nbsp;г</option><option value="7">°F</option></select>';
		is += ' <select id="extsmiley"><option value="0">&nbsp;&nbsp;&nbsp;</option><option value="1">&nbsp;^_^&nbsp;</option><option value="2">&nbsp;-&and;-&nbsp;</option><option value="3">&nbsp;&Delta;&#9651;&Delta;&nbsp;</option><option value="4">(&nbsp;&nbsp;&nbsp;)</option><option value="5">(^_^)</option><option value="6">(-&and;-)</option><option value="7" selected>(&Delta;&#9651;&Delta;)</option></select>';
	}else {
		is += '<input size="6" title="Big number: -99.5..1999.5" id="extbignumb" maxlength="5" value="123.4">';
		is += ' <select id="exttmpsmb"><option value="0">&nbsp;&nbsp;</option><option value="1">°Г</option><option value="2">&nbsp;-</option><option value="3">°F</option><option value="4">&nbsp;_</option><option value="5">°C</option><option value="6" selected>&nbsp;=</option><option value="7">°E</option></select>';
		is += ' <select id="extsmiley"><option value="0">&nbsp;&nbsp;&nbsp;</option><option value="1">&nbsp;^_^&nbsp;</option><option value="2">&nbsp;-&and;-&nbsp;</option><option value="3">&nbsp;&Delta;&#9651;&Delta;&nbsp;</option><option value="4">(&nbsp;&nbsp;&nbsp;)</option><option value="5">(^_^)</option><option value="6">(-&and;-)</option><option value="7" selected>(&Delta;&#9651;&Delta;)</option></select>';
		is += '	<input size="5" title="Small number: -9..99" id="extsmalnumb" maxlength="2" value="99">';
	}
	is += '&nbsp;%<input type="checkbox" id="extpersent">';
	is += '	<input size="5" title="Validity time = Show time in sec: 1..65535" id="extvtimed" maxlength="5" value="600">';
	is += '&nbsp;Battery:<input type="checkbox" id="extbattery">';
	is += '	<button type="button" onclick="sendCustomSetting(&quot;22&quot;);">Get OldData</button>';
	is += '	<button type="button" id="extsend" onclick="sendExt();">Show</button>';
	ext.enable = true;
	is += '<br><br><hr><br><b>Configuration:</b>&nbsp;<button type="button" onclick="sendCustomSetting(&quot;55&quot;);">Get Config</button><br><br>';
	if(cfg.ver >= 0x43)
		is += 'Screen Off: <input title="Screen Off" type="checkbox" id="cfg_scr_off"><br><br>';
	is += 'Temperature: <select id="cfg_flg_cf"><option value="0">°C</option><option value="1">°F</option></select>';
	if((hwver_id == 9) || (hwver_id == 12))  {
		is += '&nbsp;&nbsp;12-hour clock:<input type="checkbox" id="cfg_flg_am_pm">&nbsp;&nbsp;T&H 2 decimal places<input type="checkbox" id="cfg_flg_x100">';
	  if(hwver_id == 12)
		is += '<br><br>Date DD/MM: <input type="checkbox" id="cfg_flg3_ddmm">';
	} else if (hwver_id == 49) {
		is += '&nbsp;&nbsp;12-hour clock:<input type="checkbox" id="cfg_flg_am_pm"> , Show WeekDay: <input type="checkbox" id="cfg_flg3_weekday">'
	}
	is += '<br><br>';
	if((hwver_id == 2)||(hwver_id == 6)||(hwver_id == 7))
		is += 'Line: <select id="cfg_smiley"><option value="0" selected>&nbsp;&nbsp;&nbsp;</option><option value="5">&nbsp;---&nbsp;</option></select>';
	else if((hwver_id == 9) || (hwver_id == 12))
		is += 'Show: <select id="cfg_scr_type"><option value="0">Time</option><option value="1">Temperature</option><option value="2">Humidity</option><option value="3">Battery %</option><option value="4">Battery V</option><option value="5">ExtNumber&Symbols</option></select>';
	//else if(hwver_id == 49)
	//	is += 'Show: <select id="cfg_smiley"><option value="0">&nbsp;&nbsp;&nbsp;</option><option value="1">&nbsp;^_^&nbsp;</option><option value="2">&nbsp;-&and;-&nbsp;</option><option value="3">&nbsp;&Delta;&#9651;&Delta;&nbsp;</option><option value="4">(&nbsp;&nbsp;&nbsp;)</option><option value="5">(^_^)</option><option value="6">(-&and;-)</option><option value="7">(&Delta;&#9651;&Delta;)</option></select>';
	else
		is += 'Smiley: <select id="cfg_smiley"><option value="0">&nbsp;&nbsp;&nbsp;</option><option value="1">&nbsp;^_^&nbsp;</option><option value="2">&nbsp;-&and;-&nbsp;</option><option value="3">&nbsp;&Delta;&#9651;&Delta;&nbsp;</option><option value="4">(&nbsp;&nbsp;&nbsp;)</option><option value="5">(^_^)</option><option value="6">(-&and;-)</option><option value="7">(&Delta;&#9651;&Delta;)</option></select>';
	is += ', Comfort: <input type="checkbox" id="cfg_flg_comfort">';
	if((hwver_id == 9) || (hwver_id == 12)) {
	  if(cfg.ver > 0x48)
		is += ', Show WeekDay: <input type="checkbox" id="cfg_flg3_weekday">';
	} else
		is += ', Show battery: <input type="checkbox" id="cfg_flg_show_batt">';
	if(cfg.ver >= 0x18) {
		if(!((hwver_id == 9)||(hwver_id == 12)||(hwver_id == 49)))
			is += ', Clock: <input type="checkbox" id="cfg_flg_blinking"> ';
		is += '<button type="button" onclick="setDevTime();">Set Time</button> <button type="button" onclick="sendCustomSetting(&quot;23&quot;);">Get Time</button>';
	}else
		is += ', Blinking: <input type="checkbox" id="cfg_flg_blinking">';
	is += '<br><br>Sensor in "LowPower mode": <input title="Sensor measurements in Low Power mode" type="checkbox" id="cfg_flg_lp_meas">';
	is += ', Tx measures: <input title="When connected, start transferring measurements" type="checkbox" id="cfg_flg_tx_meas">';
  } else {
	is += '<br><hr><br><b>Configuration:</b>&nbsp;<button type="button" onclick="sendCustomSetting(&quot;55&quot;);">Get Config</button><br><br>';
	//is += 'Sensor in "LowPower mode": <input title="Sensor measurements in Low Power mode" type="checkbox" id="cfg_flg_lp_meas">';
	is += 'Tx measures: <input title="When connected, start transferring measurements" type="checkbox" id="cfg_flg_tx_meas">';
  }
	is += ' <button type="button" title="Start transferring measurements. Only in the current connection." onclick="sendCustomSetting(&quot;33ff&quot;);">Start Tx Measure</button>';
	is += ' <button type="button" title="Stop transferring measurements. Only in the current connection." onclick="sendCustomSetting(&quot;3300&quot;);">Stop Tx Measure</button><br><br>';
	if(cfg.ver < 0x47) {
		is += 'Temperature offset: <input size="5" title="-12.7..12.7°, default 0" id="cfg_tmp_off" maxlength="5" value="0"> °';
		is += ', Humidity offset: <input size="5" title="-12.7..12.7%, default 0" id="cfg_hm_off" maxlength="5" value="0"> %<br><br>';
	} else if(cfg.ver > 0x50) {
		is += 'Pseudo-random delay to advInterval: <input size="5" title="A pseudo-random value in the range from 0 to 10 ms is added to a fixed advInterval so that advertising events change over time. Value in 0.625 ms." id="cfg_flg3_adv_delay" maxlength="5" value="10"> x0.625 ms';
		is += ', Number of duplicates when transmitting events beacons: <input size="5" title="Range: 5..255. Number of duplicates when transmitting events beacons, default 6" id="cfg_event_adv_cnt" maxlength="5" value="6"><br><br>';
	}
	if(cfg.ver >= 0x36) {
		if(cfg.ver >= 0x42)
			is += 'BT5+ PHY: <input title="Support BT5.0+ PHY (LE 2M/1M/500K/125K, CSA2)" type="checkbox" id="bt5_flags">, LE Long Range (only for BT5.0+ adapters!): <input title="Enable Extended Advertising Long Range (Coded PHY S8)" type="checkbox" id="longrange_flags"><br><br>';
		else if(cfg.ver >= 0x40)
			is += 'BT5+ PHY: <input title="Support BT5.0+ PHY (LE 2M/1M/500K/125K, CSA2)" type="checkbox" id="bt5_flags"><br><br>';
		else
			is += 'BT5+ PHY: <input title="Support BT5.0+ PHY (LE 2M/1M/500K/125K)" type="checkbox" id="bt5_flags">, CSA2: <input title="Channel Selection Algorithm 2 or 1" type="checkbox" id="csa2_flags"><br><br>';
	}
  if(hwver_id <= 15 || (hwver_id >= 30 && hwver_id <= 33) || (hwver_id == 38) || (hwver_id == 52)) { // LCD
		is += 'Advertising type: <select id="cfg_flg_adv_type"><option value="0">ATC1441</option><option value="1">PVVX (Custom)</option><option value="2">MIJIA (MiHome)</option><option value="3" selected>';
	if(cfg.ver >= 0x37)
		if(cfg.ver >= 0x45)
			is += 'BTHome v2</option></select>';
		else
			is += 'BTHome v1</option></select>';
	else
		is += 'All</option></select>';
	if(cfg.ver >= 0x30)
		is += ', AdFlags: <input title="Toggle support for third-party software" type="checkbox" id="adv_flags">';
	if(cfg.ver >= 0x22)
		is += ', Encrypted beacon: <input title="Bindkey encrypted beacon" type="checkbox" id="cfg_adv_crypto">';
	is += '<br><br>Advertising interval: <input size="5" title="62.5 ms .. 10 000 ms, step: 62.5 ms, default 2500 ms" id="cfg_adv_int" maxlength="6" value="40"> ms, step: 62.5 ms<br><br>';
	is += 'Measure interval: <input size="5" title="1..25 (62.5 ms .. 250 sec), default 4 (10 sec)" id="cfg_meas_int" maxlength="3" value="4"> x(Advertising interval) <span id="cfg_meas_int_val"></span><br><br>';
	if(cfg.ver > 0x40)
		is += 'Connect latency: <input size="5" title="0..1000 ms, default 1000 ms" id="cfg_con_lat" maxlength="4" value="1000"> ms, step 20 ms<br><br>';
	else
		is += 'Connect latency: <input size="5" title="0..4000 ms, default 2500 ms" id="cfg_con_lat" maxlength="4" value="2500"> ms, step 20 ms<br><br>';
  } else {
	if(cfg.ver < 0x48) {
	  	is += 'Advertising type: <select id="cfg_flg_adv_type"><option value="0">BTHome v2</option><option value="1"  selected>PVVX (Custom)</option><option value="2">MIJIA (MiHome)</option><option value="3">BTHome v1</option></select>';
	} else {
		is += 'Sensor in "LowPower mode": <input title="Sensor measurements in Low Power mode" type="checkbox" id="cfg_flg_lp_meas"><br><br>';
		is += 'Advertising type: <select id="cfg_flg_adv_type"><option value="0">ATC1441</option><option value="1">PVVX (Custom)</option><option value="2">MIJIA (MiHome)</option><option value="3" selected>BTHome v2</option></select>';
	}
	is += ', AdFlags: <input title="Toggle support for third-party software" type="checkbox" id="adv_flags">';
	is += ', Encrypted beacon: <input title="Bindkey encrypted beacon" type="checkbox" id="cfg_adv_crypto">';
	is += '<br><br>Advertising interval: <input size="5" title="62.5 ms .. 10 000 ms, step: 62.5 ms, default 2500 ms" id="cfg_adv_int" maxlength="6" value="40"> ms, step: 62.5 ms<br><br>';
	is += 'Measure interval: <input size="5" title="1..25 (62.5 ms .. 250 sec), default 4 (10 sec)" id="cfg_meas_int" maxlength="3" value="4"> x(Advertising interval) <span id="cfg_meas_int_val"></span><br><br>';
	is += 'Connect latency: <input size="5" title="0..1000 ms, default 1000 ms" id="cfg_con_lat" maxlength="4" value="1000"> ms, step 20 ms<br><br>';
	//MAX_RF_TX_Power = true;
  }
	//if(MAX_RF_TX_Power) 
	is += 'RF TX Power: <select id="cfg_rf_tx"><option value="63">VBAT+10.46 dbm</option><option value="61">VBAT+10.29 dbm</option><option value="58">VBAT+10.01 dbm</option><option value="56">VBAT+9.81 dbm</option><option value="53">VBAT+9.48 dbm</option><option value="51">VBAT+9.24 dbm</option><option value="49">VBAT+8.97 dbm</option><option value="47">VBAT+8.73 dbm</option><option value="45">VBAT+8.44 dbm</option><option value="43">VBAT+8.13 dbm</option><option value="41">VBAT+7.79 dbm</option><option value="39">VBAT+7.41 dbm</option><option value="37">VBAT+7.02 dbm</option><option value="35">VBAT+6.60 dbm</option><option value="33">VBAT+6.14 dbm</option><option value="31">VBAT+5.65 dbm</option><option value="29">VBAT+5.13 dbm</option><option value="27">VBAT+4.57 dbm</option><option value="25">VBAT+3.94 dbm</option><option value="23">VBAT+3.23 dbm</option><option value="191" selected>VANT+3.01 dbm</option><option value="189">VANT+2.81 dbm</option><option value="187">VANT+2.61 dbm</option><option value="185">VANT+2.39 dbm</option><option value="182">VANT+1.99 dbm</option><option value="180">VANT+1.73 dbm</option><option value="178">VANT+1.45 dbm</option><option value="176">VANT+1.17 dbm</option><option value="174">VANT+0.90 dbm</option><option value="172">VANT+0.58 dbm</option><option value="169">VANT+0.04 dbm</option><option value="168">VANT-0.14 dbm</option><option value="164">VANT-0.97 dbm</option><option value="162">VANT-1.42 dbm</option><option value="160">VANT-1.89 dbm</option><option value="158">VANT-2.48 dbm</option><option value="156">VANT-3.03 dbm</option><option value="154">VANT-3.61 dbm</option><option value="152">VANT-4.26 dbm</option><option value="150">VANT-5.03 dbm</option><option value="148">VANT-5.81 dbm</option><option value="146">VANT-6.67 dbm</option><option value="144">VANT-7.65 dbm</option><option value="142">VANT-8.65 dbm</option><option value="140">VANT-9.89 dbm</option><option value="138">VANT-11.4 dbm</option><option value="136">VANT-13.29 dbm</option><option value="134">VANT-15.88 dbm</option><option value="132">VANT-19.27 dbm</option><option value="130">VANT-25.18 dbm</option></select><br><br>';
	//else is += 'RF TX Power: <select id="cfg_rf_tx"><option value="191" selected>VANT+3.01 dbm</option><option value="189">VANT+2.81 dbm</option><option value="187">VANT+2.61 dbm</option><option value="185">VANT+2.39 dbm</option><option value="182">VANT+1.99 dbm</option><option value="180">VANT+1.73 dbm</option><option value="178">VANT+1.45 dbm</option><option value="176">VANT+1.17 dbm</option><option value="174">VANT+0.90 dbm</option><option value="172">VANT+0.58 dbm</option><option value="169">VANT+0.04 dbm</option><option value="168">VANT-0.14 dbm</option><option value="164">VANT-0.97 dbm</option><option value="162">VANT-1.42 dbm</option><option value="160">VANT-1.89 dbm</option><option value="158">VANT-2.48 dbm</option><option value="156">VANT-3.03 dbm</option><option value="154">VANT-3.61 dbm</option><option value="152">VANT-4.26 dbm</option><option value="150">VANT-5.03 dbm</option><option value="148">VANT-5.81 dbm</option><option value="146">VANT-6.67 dbm</option><option value="144">VANT-7.65 dbm</option><option value="142">VANT-8.65 dbm</option><option value="140">VANT-9.89 dbm</option><option value="138">VANT-11.4 dbm</option><option value="136">VANT-13.29 dbm</option><option value="134">VANT-15.88 dbm</option><option value="132">VANT-19.27 dbm</option><option value="130">VANT-25.18 dbm</option></select><br><br>';
  if(hwver_id <= 15  || (hwver_id >= 30 && hwver_id <= 33) || (hwver_id == 38) || (hwver_id == 49) || (hwver_id == 52)) { 
	if(!((hwver_id == 9)||(hwver_id == 12)||(hwver_id == 49)))
		is += 'Minimum LCD refresh rate: <input size="5" title="0.5..12.75 sec, step: 0.05 s, default 2.45 s" id="cfg_lcd_tint" maxlength="6" value="2.45"> s, step: 0.05 s<br><br>';
	if(cfg.ver == 0x19) {
		is += 'Recording measurements to flash memory <input type="checkbox" id="cfg_flg2_memo" title="Recording measurements to flash memory (cyclic buffer for 19632 measurements)">';
		is += ' <button type="button" onclick="sendGetMemo(50);">Get the last 50 records</button> <a href="GraphMemo.html" target="_blank">GraphMemo.html</a><br><br>';
	}
	if(cfg.ver >= 0x20) {
		is += 'Recording averaging measurements to flash memory <input size="5" title="0..255, =0 - off, default 60. Set time/date!" id="cfg_av_meas_mem" maxlength="3" value="60"> x(measure interval) <span id="cfg_av_meas_mem_val"></span><br>';
		if(cfg.ver >= 0x23)
			is += '<button type="button" onclick="sendCustomSetting(&quot;36123400&quot;);">!Delete all records!</button> '
		is += '<a href="GraphMemo.html" target="_blank">GraphMemo.html</a> <button type="button" onclick="setDevTime();">Set Time</button> <button type="button" onclick="sendGetMemo(50);">Get the last 50 records</button><br><br>';
	}
	is += '<button type="button" onclick="SendCustomConfig()">Send Config</button>';
	is += ' <button type="button" onclick="sendCustomSetting(&quot;56&quot;);">Set default</button><br>';
	if(cfg.ver >= 0x47)
		is += '<hr><label id="lblSensor"><b>Sensor Settings:</b></label><br>Temperature Slope factor: <input size="8" type="text" id="inpTempK" maxlength="16" title="Tk: Slope factor (linear function) for temperature calculation (T=RegT*Tk/65536+Tz)"> , Zero offset: <input size="8" type="text" id="inpTempZ" maxlength="16" title="Tz: Zero offset with correction value for temperature calculation (T=RegT*Tk/65536+Tz)"><br>Humidity Slope factor: <input size="8" type="text" id="inpHumK" maxlength="16" title="Hk: Slope factor (linear function) for humidity calculation (H=RegH*Hk/65536+Hz)"> , Zero offset: <input size="8" type="text" id="inpHumZ" maxlength="16" title="Hz: Zero offset with correction value for humidity calculation (H=RegH*Hk/65536+Hz)"><br><button type="button"id="btnGetSens" onclick="getSensCfg()">Get Sensor Settings</button> <button type="button"id="btnSetSens" onclick="setSensCfg()">Send Sensor Settings</button> <button type="button"id="btnRstSens" onclick="resetSensCfg()" title="Restore default sensor settings">Set Default</button><br>';

	//if(devSens.sentype && devSens.sentype == 14)
	//	is += '<hr><label id="lblSensorSCD41"><b>SCD41 Settings:</b></label><br>Altitude: <input size="8" type="text" id="inpSCD4xAltitude" maxlength="16"> , Pressure: <input size="8" type="text" id="inpSCD4xPressure" maxlength="16"><br><br><label><b>Automatic Self Calibration:</b><br></label> Enabled: <input type="checkbox" id="chbSDC41asc">, ASC CO2: <input size="8" type="text" id="inpSCD4xAscCo2" maxlength="16"><br> Initial Period: <input size="8" type="text" id="inpSCD4xInitPeriod" maxlength="16"> , Standard Period: <input size="8" type="text" id="inpSCD4xStdPeriod" maxlength="16"><br><button type="button"id="btnGetSensSCD4x" onclick="getSensSCD4x()">Get Sensor SCD41 Settings</button> <button type="button"id="btnSetSensSCD4x" onclick="setSensSCD4x()">Send Sensor SCD41 Settings</button><br><label><b>Functions:</b></label><br><button type="button" id="btnf1SensSCD4x" onclick="funSensSCD4x(1)" >Forced Recalibration:</button> <input size="8" type="text" id="inpSCD4xPFRecal" maxlength="16"> ppm<br><button type="button" id="btnf2SensSCD4x" title="Stores the current configuration in the EEPROM of the SCD4x" onclick="funSensSCD4x(2)" >Persist Settings</button><br>';
	/* --- */
	if((hwver_id == 17) || (hwver_id == 27))
		is += '<hr><label id="lblSensor2"><b>Sensor2 Settings:</b></label><br>Temperature Slope factor: <input size="8" type="text" id="inpTemp2K" maxlength="16" title="Tk: Slope factor (linear function) for temperature calculation (T=RegT*Tk/65536+Tz)"> , Zero offset: <input size="8" type="text" id="inpTemp2Z" maxlength="16" title="Tz: Zero offset with correction value for temperature calculation (T=RegT*Tk/65536+Tz)"><br><button type="button"id="btnGetSens2" onclick="getSens2Cfg()">Get Sensor2 Settings</button> <button type="button"id="btnSetSens2" onclick="setSens2Cfg()">Send Sensor2 Settings</button> <button type="button"id="btnRstSens2" onclick="resetSens2Cfg()" title="Restore default sensor2 settings">Set Default</button><br>';

	if(cfg.ver >= 0x24 && hwver_id != 9 && hwver_id != 12 && hwver_id != 49) {
		is += '<hr><button type="button" title="Get time clock delta" onclick="sendCustomSetting(&quot;24&quot;);">Get delta time</button>';
		is += ' Adjust time clock delta: <input size="5" title="-32767..32767, in 1/16 us for 1 sec, default 0" id="cfg_time_step" maxlength="16" value="0">';
		is += ' <button type="button" onclick="sendDeltaTime();">Set delta time</button>';
		if(cfg.ver >= 0x36)
			is += ' <button type="button" title="The minimum period for calculation is 3 hours!" onclick="calkDeltaTime();">Calk delta time</button><br>';
		else
			is += '<br>';
	}
	if((hwver_id == 0)||(hwver_id == 3)||(hwver_id == 4)||(hwver_id == 5)||(hwver_id == 10)||(hwver_id == 14)) { // LYWSD03MMC
		if(cfg.ver >= 0x53) {
			is += '<hr><b>Transmit and scan TimeStamp</b>';
			is += '<br>Interval: <input size="8" title="0 - Off, 1..4294967295 sec, default 0" id="trsc_int" maxlength="12" value="0"> sec, ';
			is +='ScanDevice MAC: <input size="40" maxlength="16" type="text" id="trsc_mac" title="Must be 6 hex MAC digits!" value="">';
			is += '<br> <button type="button" title="Get/read TrScIn setings" onclick="getTrScConfig();">Get</button>';
			is += ' <button type="button" title="Set/save TrScIn setings" onclick="setTrScConfig();">Set</button><br>';
		}
		is += '<hr><b>Management GPIO_TRG</b> (mark "reset"):';
	} else
		is += '<hr><b>Management GPIO_TRG:</b>';
	is += '&nbsp;<button type="button" title="Get/read current setings" onclick="sendCustomSetting(&quot;44&quot;);">Get TRG</button><br>';
	if(cfg.ver >= 0x26) {
		is += 'Temperature hysteresis: <input size="8" title="Step 0.01°, default -0.55°, =0 off, if less than zero - activation on decrease, if more than zero - activation on excess" id="trg_tmp_hst" maxlength="7" value="-0.55"> °';
		is += ', Humidity hysteresis: <input size="8" title="Step 0.01%, default 0, =0 off, if less than zero - activation on decrease, if more than zero - activation on excess" id="trg_hm_hst" maxlength="7" value="0.0"> %<br>';
	} else {
		is += 'Temperature hysteresis: <input size="8" title="-12.7..12.7°, default -0.1, =0 off, if less than zero - activation on decrease, if more than zero - activation on excess" id="trg_tmp_hst" maxlength="5" value="-0.1"> °';
		is += ', Humidity hysteresis: <input size="8" title="-12.7..12.7%, default 0, =0 off, if less than zero - activation on decrease, if more than zero - activation on excess" id="trg_hm_hst" maxlength="5" value="0.0"> %<br>';
	}
	is += 'Temperature threshold: <input size="8" title="-40.00..80.00°C, default 21.00°C" id="trg_tmp_thr" maxlength="6" value="21.00"> °';
	is += ', Humidity threshold: <input size="8" title="0..99.00%, default 50.00%" id="trg_hm_thr" maxlength="6" value="50.00"> %<br><br>';
	if(cfg.ver >= 0x37) {
		is += '<b>Management GPIO_RS (Reed Switch):</b><br><br>';
		is += 'RS mode: <select id="rds_type"><option value="0" selected>None</option><option value="1">Switch</option><option value="2">Counter</option>';
		if(cfg.ver >= 0x42 && hwver_id != 9 && hwver_id != 12)
			is += '<option value="3" selected>Connect</option>';
		if(cfg.ver >= 0x39)	is += '</select>, Invert RS event: <input type="checkbox" id="rds_invert">';
		is += '<br>RS report interval: <input size="8" title="0 - Off, 1..65535 sec, default 3600 sec" id="rds_rpint" maxlength="6" value="3600"> sec';
	}
	is += '<br> <button type="button" title="Set/save current setings" onclick="sendTrg();">Set TRG</button>';
	is += ' <button type="button" title="GPIO_TRG PullUp 10 kOm. Work only TRG off!" onclick="sendCustomSetting(&quot;4501&quot;);">Set pin to "1"</button>';
	is += ' <button type="button" title="GPIO_TRG PullDown 100 kOm. Work only TRG off!" onclick="sendCustomSetting(&quot;4500&quot;);">Set pin to "0"</button><br><hr>';
	if(cfg.ver >= 0x13 && hwver_id != 31 && hwver_id != 38) {
		is += '<b>Comfort parameters:</b>&nbsp;';
		is += '<button type="button" title="Get current comfort parameters. The parameters are processed if the configuration is set to &quot;Comfort: On&quot;." onclick="sendCustomSetting(&quot;20&quot;);">Get current comfort parameters</button><br>';
		is += 'Temperature Lo:<input size="8" title="-40.00..125.00°C, default 21.00°C" id="cmf_tmp_lo" maxlength="6" value="21.00">, ';
		is += 'Hi: <input size="8" title="-40.00..125.00°C, default 26.00°C" id="cmf_tmp_hi" maxlength="6" value="26.00"> °C<br>';
		is += 'Humidity Lo:<input size="8" title="0.0..99.99%, default 30.00" id="cmf_hm_lo" maxlength="5" value="30.00">, ';
		is += 'Hi:<input size="8" title="0.0..99.99%, default 60.00%" id="cmf_hm_hi" maxlength="5" value="60.00"> %<br>';
		is += '<button type="button" title="Set comfort parameters" onclick="SendCmf();">Set comfort parameters</button><br><hr>';
		cmf.enable = true;
	}
	trg.enable = true;
	if(hwver_id < 30 || hwver_id == 49)
		is += '<b>Keys and Codes:</b>&nbsp;<button type="button" onclick="sendCustomSetting(&quot;15&quot;);">Show all mi keys</button><br>';
	if(cfg.ver >= 0x11) {
		is += '<input size="6" maxlength="6" type="text" title="000000..999999, 6 decimal digits, 000000 - PinCode Disable. Warning: If the PinCode is forgotten - only a hardware flasher!" id="pincode" value="000000"> <button title="Warning: If the PinCode is forgotten - only a hardware flasher!" type="button" onclick="sendPinCode();">!Set PinCode!</button><br>';
		pincode.enable = true;
		if(cfg.ver >= 0x14) {
			is += '<br><button type="button" onclick="sendCustomSetting(&quot;01&quot;);">Get device Name</button> <input size="10" maxlength="18" type="text" title="Device Name: 1..18 chars" id="dev_name" value=""> <button type="button" title="Set New Name" onclick="sendDevName();">Set New Name</button> <button title="Clean Name: ATC_xxxx" type="button" onclick="CleanDevName();">Set default Name</button><br>';
			dnm.enable = true;
		}
	}
	if(mikeys.mac) {
		is +='Device MAC [+ 2 Rand]:<br><input size="40" maxlength="16" type="text" id="mi_mac" title="Must be 6 hex MAC digits [+ 2 hex RandMAC digits]!" value="'+hex(mikeys.mac[5],2)+hex(mikeys.mac[4],2)+hex(mikeys.mac[3],2)+hex(mikeys.mac[2],2)+hex(mikeys.mac[1],2)+hex(mikeys.mac[0],2)+hex(mikeys.mac[7],2)+hex(mikeys.mac[6],2)+'">';
		// ', C0'+hex(mikeys.mac[7],2)+hex(mikeys.mac[6],2)+hex(mikeys.mac[2],2)+hex(mikeys.mac[1],2)+hex(mikeys.mac[0],2)'
		if(cfg.ver >= 0x14)
			is +=' <button type="button" title="Set Custom MAC. Warning: If the pin code is set, restart all Soft & Hard for the next connection!" onclick="sendMAC();">!Set MAC!</button> <button type="button" title="Clean MAC - Set Standart Random MAC: A4C138******. Warning: If the pin code is set, restart all Soft & Hard for the next connection!" onclick="CleanMAC();">!Clean MAC!</button>';
		is +='<br>';
	} else {
		is +='Device MAC [+ 2 Rand]:<br><button type="button" title="Get MAC" onclick="sendCustomSetting(&quot;10&quot;);">Get MAC</button> <input size="40" maxlength="16" type="text" id="mi_mac" title="Must be 6 hex MAC digits [+ 2 hex RandMAC digits]!" value="?">';
		is +=' <button type="button" title="Set Custom MAC. Warning: If the pin code is set, restart all Soft & Hard for the next connection!" onclick="sendMAC();">!Set MAC!</button> <button type="button" title="Clean MAC - Set Standart Random MAC: A4C138******. Warning: If the pin code is set, restart all Soft & Hard for the next connection!" onclick="CleanMAC();">!Clean MAC!</button><br>';
	}
	if(mikeys.id) {
		let str = new TextDecoder("utf-8").decode(mikeys.id.slice(1));
		is +='Device known id:<br><input size="40" maxlength="19" type="text" id="known_id" value="'+str+'"><br>';
	}
	if(mikeys.bindkey && mikeys.token) {
		is +='Mi Token:<br><input size="40" maxlength="24" type="text" id="mi_token" value="'+dump(mikeys.token,12)+'"><br>';
		is +='Mi Bind Key:<br><input size="40" maxlength="32" type="text" id="mi_bind_key" value="'+dump(mikeys.bindkey,16)+'">';
		is +=' <button type="button" title="Set new Mi Token and Bind keys" onclick="setNewTBKey();">Set new Token & Bind keys</button><br>';
		if(cfg.ver >= 0x21)
			is +=' <button type="button" title="Erase all MiKeys" onclick="sendCustomSetting(&quot;17&quot;);">!Erase all Mi Keys!</button>';
		if(mikeys.restore) {
			is += '<br><button type="button" onclick="sendCustomSetting(&quot;16&quot;);">Swap previous token + bindkey</button>';
		}
		is += '<br>';
	}
	if(cfg.ver >= 0x28) {
		is +='BindKey:<br><input size="40" maxlength="32" title="Bind Key must be 32 hex characters (16 bytes)" type="text" id="cbind_key" value="?">';
		is +=' <button type="button" title="Get BindKey" onclick="getBKey();">Get BindKey</button>';
		is +=' <button type="button" title="Set new BindKey" onclick="setBKey();">Set BindKey</button><br>';
	}
	$("custcfg").innerHTML = is;
	if(cfg.ver >= 0x43)
		$("cfg_scr_off").checked = (cfg.flg2&0x80) != 0
	$("cfg_flg_adv_type").value = cfg.flg&3;
	$("cfg_flg_comfort").checked = (cfg.flg&4) != 0;
	if((hwver_id == 9) || (hwver_id == 12)) {
		$("cfg_flg_am_pm").checked = (cfg.flg&32) != 0;
		$("cfg_flg_x100").checked = (cfg.flg&8) != 0;
		$("cfg_scr_type").value = cfg.flg2&7;
	    if(cfg.ver > 0x48) {
	    	$("cfg_flg3_weekday").checked = ((cfg.temp_offset&128) == 0);
		    if(hwver_id == 12)
		    	$("cfg_flg3_ddmm").checked = ((cfg.temp_offset&64) != 0);
		}
		$("adv_flags").checked = (cfg.flg2&0x10) != 0;
		$("bt5_flags").checked = (cfg.flg2&0x20) != 0;
		$("longrange_flags").checked = (cfg.flg2&0x40) != 0;
	} else if(hwver_id == 49) {
		$("cfg_flg_am_pm").checked = (cfg.flg&32) != 0;
		$("cfg_flg_show_batt").checked = (cfg.flg&8) != 0;
		$("cfg_smiley").value = cfg.flg2&7;
    	$("cfg_flg3_weekday").checked = ((cfg.temp_offset&128) != 0);
		$("adv_flags").checked = (cfg.flg2&0x10) != 0;
		$("bt5_flags").checked = (cfg.flg2&0x20) != 0;
		$("longrange_flags").checked = (cfg.flg2&0x40) != 0;
	} else {
		$("cfg_flg_blinking").checked = (cfg.flg&8) != 0;
		$("cfg_flg_show_batt").checked = (cfg.flg&32) != 0;
		$("cfg_smiley").value = cfg.flg2&7;
		if(cfg.ver == 0x19)
			$("cfg_flg2_memo").checked = (cfg.flg2&8) != 0;
		if(cfg.ver >= 0x30)
			$("adv_flags").checked = (cfg.flg2&0x10) != 0;
		if(cfg.ver >= 0x36) {
			$("bt5_flags").checked = (cfg.flg2&0x20) != 0;
			if(cfg.ver >= 0x42)
				$("longrange_flags").checked = (cfg.flg2&0x40) != 0;
			else if(cfg.ver < 0x40)
				$("csa2_flags").checked = (cfg.flg2&0x40) != 0;
		}
		$("cfg_lcd_tint").value = String((cfg.lcd_tint * 0.05).toFixed(2));
	}
	$("cfg_flg_cf").value = ((cfg.flg&16) != 0 ? 1 : 0);
	$("cfg_flg_tx_meas").checked = (cfg.flg&64) != 0;
	$("cfg_flg_lp_meas").checked = (cfg.flg&128) != 0;
	if(cfg.ver >= 0x22)
		$("cfg_adv_crypto").checked = (cfg.flg2&0x08) != 0;
	if(cfg.ver >= 0x20) {
		$("cfg_av_meas_mem").value = String(cfg.av_meas_mem);
		$("cfg_av_meas_mem").onchange = function() {InfoIntervals();};
	}
	if(cfg.ver < 0x47) {
		$("cfg_tmp_off").value = String((cfg.temp_offset / 10.0).toFixed(1));
		$("cfg_hm_off").value = String((cfg.humi_offset / 10.0).toFixed(1));
	} else if(cfg.ver > 0x50) {
		$("cfg_flg3_adv_delay").value = String(cfg.temp_offset&15);
		$("cfg_event_adv_cnt").value = String(cfg.humi_offset);
	}
	$("cfg_adv_int").value = String((cfg.advertising_interval * 62.5).toFixed(1));
	$("cfg_adv_int").onchange = function() {InfoIntervals();};
	$("cfg_meas_int").value = String(cfg.measure_interval);
	$("cfg_meas_int").onchange = function() {InfoIntervals();};
	$("cfg_rf_tx").value = String(cfg.rf_tx_power);
	$("cfg_con_lat").value = String((cfg.connect_latency + 1) * 20);
  } else {
	is +='Recording averaging measurements to flash memory <input size="5" title="0..255, =0 - off, default 60. Set time/date!" id="cfg_av_meas_mem" maxlength="3" value="60"> x(measure interval) <span id="cfg_av_meas_mem_val"></span><br>';
	is +='<button type="button" onclick="sendCustomSetting(&quot;36123400&quot;);">!Delete all records!</button> '
	is +='<a href="GraphMemo.html" target="_blank">GraphMemo.html</a> <button type="button" onclick="setDevTime();">Set Time</button> <button type="button" onclick="sendGetMemo(50);">Get the last 50 records</button><br><br>';
	is +='<button type="button" onclick="SendCustomConfig()">Send Config</button>';
	is +=' <button type="button" onclick="sendCustomSetting(&quot;56&quot;);">Set default</button><br>';
	is +='<hr><button type="button" title="Get time clock delta" onclick="sendCustomSetting(&quot;24&quot;);">Get delta time</button>';
	is +=' Adjust time clock delta: <input size="5" title="-32767..32767, in 1/16 us for 1 sec, default 0" id="cfg_time_step" maxlength="6" value="0">';
	is +=' <button type="button" onclick="sendDeltaTime();">Set delta time</button>';
	is +=' <button type="button" title="The minimum period for calculation is 3 hours!" onclick="calkDeltaTime();">Calk delta time</button><br>';
	if(cfg.ver >= 0x47)
		is += '<hr><label id="lblSensor"><b>Sensor Settings:</b></label><br>Temperature Slope factor: <input size="8" type="text" id="inpTempK" maxlength="8" title="Tk: Slope factor (linear function) for temperature calculation (T=RegT*Tk/65536+Tz)"> , Zero offset: <input size="8" type="text" id="inpTempZ" maxlength="8" title="Tz: Zero offset with correction value for temperature calculation (T=RegT*Tk/65536+Tz)"><br>Humidity Slope factor: <input size="8" type="text" id="inpHumK" maxlength="8" title="Hk: Slope factor (linear function) for humidity calculation (H=RegH*Hk/65536+Hz)"> , Zero offset: <input size="8" type="text" id="inpHumZ" maxlength="8" title="Hz: Zero offset with correction value for humidity calculation (H=RegH*Hk/65536+Hz)"><br><button type="button"id="btnGetSens" onclick="getSensCfg()">Get Sensor Settings</button> <button type="button"id="btnSetSens" onclick="setSensCfg()">Send Sensor Settings</button> <button type="button"id="btnRstSens" onclick="resetSensCfg()" title="Restore default sensor settings">Set Default</button><br><div id="addSensors" disabled></div>';
   	if(cfg.ver >= 0x48) {
		is += '<hr><b>Management GPIO_TRG:</b>';
		is += '&nbsp;<button type="button" title="Get/read current setings" onclick="sendCustomSetting(&quot;44&quot;);">Get TRG</button><br>';
		is += 'Temperature hysteresis: <input size="8" title="Step 0.01°, default -0.55°, =0 off, if less than zero - activation on decrease, if more than zero - activation on excess" id="trg_tmp_hst" maxlength="7" value="-0.55"> °';
		is += ', Humidity hysteresis: <input size="8" title="Step 0.01%, default 0, =0 off, if less than zero - activation on decrease, if more than zero - activation on excess" id="trg_hm_hst" maxlength="7" value="0.0"> %<br>';
		is += 'Temperature threshold: <input size="8" title="-40.00..80.00°C, default 21.00°C" id="trg_tmp_thr" maxlength="6" value="21.00"> °';
		is += ', Humidity threshold: <input size="8" title="0..99.00%, default 50.00%" id="trg_hm_thr" maxlength="6" value="50.00"> %<br><br>';
		is += '<b>Management GPIO_RS (Reed Switch):</b><br><br>';
		if(hwver_id == 18) {
			is += 'RS1 mode: <select id="rds_type"><option value="0" selected>None</option><option value="1">Switch</option><option value="2">Counter</option>';
			is += '</select>, Invert RS1 event: <input type="checkbox" id="rds_invert">';
			is += '<br>RS2 mode: <select id="rds2_type"><option value="0" selected>None</option><option value="1">Switch</option><option value="2">Counter</option>';
			is += '</select>, Invert RS2 event: <input type="checkbox" id="rds2_invert">';
			trg.rds2_enable = true;
		} else {
			is += 'RS mode: <select id="rds_type"><option value="0" selected>None</option><option value="1">Switch</option><option value="2">Counter</option>';
			is += '</select>, Invert RS event: <input type="checkbox" id="rds_invert">';
			trg.rds2_enable = false;
		}
		is += '<br>RS report interval: <input size="8" title="0 - Off, 1..65535 sec, default 3600 sec" id="rds_rpint" maxlength="6" value="3600"> sec';
		is += '<br> <button type="button" title="Set/save current setings" onclick="sendTrg();">Set TRG</button>';
		is += ' <button type="button" title="GPIO_TRG PullUp 10 kOm. Work only TRG off!" onclick="sendCustomSetting(&quot;4501&quot;);">Set pin to "1"</button>';
		is += ' <button type="button" title="GPIO_TRG PullDown 100 kOm. Work only TRG off!" onclick="sendCustomSetting(&quot;4500&quot;);">Set pin to "0"</button><br><hr>';
		trg.enable = true;

   	}
	is +='<hr><b>Keys and Codes:</b><br><br>EEP BindKey:<br><input size="40" maxlength="32" title="Bind Key must be 32 hex characters (16 bytes)" type="text" id="cbind_key" value="?">';
	is +=' <button type="button" title="Get EEP BindKey" onclick="getBKey();">Get EEP BindKey</button>';
	is +=' <button type="button" title="Set new EEP BindKey" onclick="setBKey();">Set EEP BindKey</button><br>';
	is += '<input size="6" maxlength="6" type="text" title="000000..999999, 6 decimal digits, 000000 - PinCode Disable. Warning: If the PinCode is forgotten - only a hardware flasher!" id="pincode" value="000000"> <button title="Warning: If the PinCode is forgotten - only a hardware flasher!" type="button" onclick="sendPinCode();">!Set PinCode!</button><br>';
	pincode.enable = true;
	is += '<br><button type="button" onclick="sendCustomSetting(&quot;01&quot;);">Get device Name</button> <input size="10" maxlength="10" type="text" title="Device Name: 1..10 chars" id="dev_name" value=""> <button type="button" title="Set New Name" onclick="sendDevName();">Set New Name</button> <button title="Clean Name: DEV_xxxx" type="button" onclick="CleanDevName();">Set default Name</button><br><br>';
	dnm.enable = true;
	is +='Device MAC [+ 2 Rand]:<br><button type="button" title="Get MAC" onclick="sendCustomSetting(&quot;10&quot;);">Get MAC</button> <input size="40" maxlength="16" type="text" id="mi_mac" title="Must be 6 hex MAC digits [+ 2 hex RandMAC digits]!" value="?">';
	is +=' <button type="button" title="Set Custom MAC. Warning: If the pin code is set, restart all Soft & Hard for the next connection!" onclick="sendMAC();">!Set MAC!</button> <button type="button" title="Clean MAC - Set Standart Random MAC: A4C138******. Warning: If the pin code is set, restart all Soft & Hard for the next connection!" onclick="CleanMAC();">!Clean MAC!</button><br>';
	$("custcfg").innerHTML = is;
	$("cfg_flg_adv_type").value = cfg.flg&3;
	$("adv_flags").checked = (cfg.flg2&0x10) != 0;
	$("bt5_flags").checked = (cfg.flg2&0x20) != 0;
	$("longrange_flags").checked = (cfg.flg2&0x40) != 0;
	$("cfg_flg_tx_meas").checked = (cfg.flg&64) != 0;
	if(cfg.ver >= 0x48)
		$("cfg_flg_lp_meas").checked = (cfg.flg&128) != 0;
	$("cfg_adv_crypto").checked = (cfg.flg2&0x08) != 0;
	$("cfg_av_meas_mem").value = String(cfg.av_meas_mem);
	$("cfg_av_meas_mem").onchange = function() {InfoIntervals();};
	if(cfg.ver < 0x47) {
		$("cfg_tmp_off").value = String((cfg.temp_offset / 10.0).toFixed(1));
		$("cfg_hm_off").value = String((cfg.humi_offset / 10.0).toFixed(1));
	} else if(cfg.ver > 0x50) {
		$("cfg_flg3_adv_delay").value = String(cfg.temp_offset&15);
		$("cfg_event_adv_cnt").value = String(cfg.humi_offset);
	}
	$("cfg_adv_int").value = String((cfg.advertising_interval * 62.5).toFixed(1));
	$("cfg_adv_int").onchange = function() {InfoIntervals();};
	$("cfg_meas_int").value = String(cfg.measure_interval);
	$("cfg_meas_int").onchange = function() {InfoIntervals();};
	$("cfg_rf_tx").value = String(cfg.rf_tx_power);
	$("cfg_con_lat").value = String((cfg.connect_latency + 1) * 20);
  }
	InfoIntervals();
	cfg.enable = true;
 } else
	$("custcfg").innerHTML = '<hr><br><br>Unsupported device software version! Custom ATC version >= 1.0!<br><br>';
}
function hex(number, len) {
	var str = (number.toString(16)).toUpperCase();
	while (str.length < len) str = '0' + str;
	return str;
}
function SendCustomConfig() {
  if(hwver_id < 15 || (hwver_id >= 30 && hwver_id <= 33) || (hwver_id == 38) || (hwver_id == 49) || (hwver_id == 52)) {
	if(hwver_id == 49) {
		cfg.flg = ($("cfg_flg_adv_type").value & 3)+
		(($("cfg_flg_comfort").checked) ? 4 : 0) +
		(($("cfg_flg_show_batt").checked != 0) ? 8 : 0) +
		(($("cfg_flg_cf").value != 0) ? 16 : 0) +
		(($("cfg_flg_am_pm").checked) ? 32 : 0) +
		(($("cfg_flg_tx_meas").checked) ? 64 : 0) +
		(($("cfg_flg_lp_meas").checked) ? 128 : 0);
		cfg.flg2 = (parseInt($("cfg_smiley").value) & 7);
		cfg.flg2 += (($("cfg_adv_crypto").checked) ? 0x08 : 0);
		cfg.flg2 += (($("adv_flags").checked )? 0x10 : 0);
		cfg.flg2 += (($("bt5_flags").checked) ? 0x20 : 0);
		cfg.flg2 += (($("longrange_flags").checked) ? 0x40 : 0);
		cfg.flg2 += (($("cfg_scr_off").checked) ? 0x80 : 0);
		cfg.temp_offset = ($("cfg_flg3_weekday").checked) ? 128 : 0;
	} else if(hwver_id == 9 || hwver_id == 12) {
		cfg.flg = ($("cfg_flg_adv_type").value & 3)+
		(($("cfg_flg_comfort").checked) ? 4 : 0) +
		(($("cfg_flg_x100").checked != 0) ? 8 : 0) +
		(($("cfg_flg_cf").value != 0) ? 16 : 0) +
		(($("cfg_flg_am_pm").checked) ? 32 : 0) +
		(($("cfg_flg_tx_meas").checked) ? 64 : 0) +
		(($("cfg_flg_lp_meas").checked) ? 128 : 0);
		cfg.flg2 = (parseInt($("cfg_scr_type").value) & 7);
		cfg.flg2 += (($("cfg_adv_crypto").checked) ? 0x08 : 0);
		cfg.flg2 += (($("adv_flags").checked )? 0x10 : 0);
		cfg.flg2 += (($("bt5_flags").checked) ? 0x20 : 0);
		cfg.flg2 += (($("longrange_flags").checked) ? 0x40 : 0);
		if(cfg.ver >= 0x43)	cfg.flg2 += (($("cfg_scr_off").checked) ? 0x80 : 0);
		if(cfg.ver > 0x48) { 
			cfg.temp_offset = ($("cfg_flg3_weekday").checked) ? 0 : 128;
	  		if(hwver_id == 12)
		  		cfg.temp_offset += ($("cfg_flg3_ddmm").checked) ? 64 : 0;
		}
	} else {
		cfg.flg = ($("cfg_flg_adv_type").value & 3)+
		(($("cfg_flg_comfort").checked) ? 4 : 0) +
		(($("cfg_flg_blinking").checked) ? 8 : 0) +
		(($("cfg_flg_cf").value != 0) ? 16 : 0) +
		(($("cfg_flg_show_batt").checked) ? 32 : 0) +
		(($("cfg_flg_tx_meas").checked) ? 64 : 0) +
		(($("cfg_flg_lp_meas").checked) ? 128 : 0);
		cfg.flg2 = (parseInt($("cfg_smiley").value) & 7);
		if(cfg.ver == 0x19)	cfg.flg2 += (($("cfg_flg2_memo").checked) ? 8 : 0);
		if(cfg.ver >= 0x22)	cfg.flg2 += (($("cfg_adv_crypto").checked) ? 0x08 : 0);
		if(cfg.ver >= 0x30)	cfg.flg2 += (($("adv_flags").checked )? 0x10 : 0);
		if(cfg.ver >= 0x36) {
			cfg.flg2 += (($("bt5_flags").checked) ? 0x20 : 0);
			if(cfg.ver >= 0x42) {
				cfg.flg2 += (($("longrange_flags").checked) ? 0x40 : 0);
				if(cfg.ver >= 0x43)	cfg.flg2 += (($("cfg_scr_off").checked) ? 0x80 : 0);
			} else if(cfg.ver < 0x40) cfg.flg2 += (($("csa2_flags").checked) ? 0x40 : 0);
		}
		cfg.lcd_tint = Math.round(parseFloat($("cfg_lcd_tint").value) / 0.05);
		if(cfg.lcd_tint < 10) cfg.lcd_tint = 10;
		else if(cfg.lcd_tint > 255) cfg.lcd_tint = 255;
	}
 } else {
		cfg.flg = (cfg.flg & 0xbc) + ($("cfg_flg_adv_type").value & 3)+(($("cfg_flg_tx_meas").checked) ? 64 : 0);
		if(cfg.ver >= 0x48)	cfg.flg = (cfg.flg & 0x7f) + (($("cfg_flg_lp_meas").checked) ? 128 : 0);
		cfg.flg2 = cfg.flg2 & 0x87;
		cfg.flg2 += (($("cfg_adv_crypto").checked) ? 0x08 : 0);
		cfg.flg2 += (($("adv_flags").checked )? 0x10 : 0);
		cfg.flg2 += (($("bt5_flags").checked) ? 0x20 : 0);
		cfg.flg2 += (($("longrange_flags").checked) ? 0x40 : 0);
 }
	if(cfg.ver >= 0x20)	cfg.av_meas_mem = parseInt($("cfg_av_meas_mem").value);
	if(cfg.ver < 0x47) {
		cfg.temp_offset = Math.round(10.0 * parseFloat($("cfg_tmp_off").value));
		if(cfg.temp_offset < -127) cfg.temp_offset = -127;
		else if(cfg.temp_offset > 127) cfg.temp_offset = 127;
		cfg.humi_offset = Math.round(10.0 * parseFloat($("cfg_hm_off").value));
		if(cfg.humi_offset < -127) cfg.humi_offset = -127;
		else if(cfg.humi_offset > 127) cfg.humi_offset = 127;
	} else if(cfg.ver > 0x50) {
		let adv_delay = parseInt($("cfg_flg3_adv_delay").value);
		if (adv_delay < 0) adv_delay = 0;
		else if (adv_delay > 15) adv_delay = 15;
		cfg.temp_offset = adv_delay | (cfg.temp_offset & 0x0f0);
		let event_adv_cnt = parseInt($("cfg_event_adv_cnt").value);
		if(event_adv_cnt < 6) event_adv_cnt = 5;
		else if(event_adv_cnt > 255) event_adv_cnt = 255;
		cfg.humi_offset = event_adv_cnt;
	}
	cfg.advertising_interval = Math.round(parseFloat($("cfg_adv_int").value) / 62.5)
	if(cfg.advertising_interval < 1) cfg.advertising_interval = 1;
	else if(cfg.advertising_interval > 255) cfg.advertising_interval = 255;
	cfg.measure_interval = parseInt($("cfg_meas_int").value);
	if(cfg.measure_interval < 1) cfg.measure_interval = 1;
	else if(cfg.measure_interval > 255) cfg.measure_interval = 255;
	cfg.rf_tx_power = parseInt($("cfg_rf_tx").value);
	if(cfg.rf_tx_power&0x80) { /*VANT*/
		if(cfg.rf_tx_power > 191) cfg.rf_tx_power = 191;
		else if(cfg.rf_tx_power < 130) cfg.rf_tx_power = 130;
	} else { /*VBAT*/
		if(cfg.rf_tx_power < 23) cfg.rf_tx_power = 23;
		else if(cfg.rf_tx_power > 63) cfg.rf_tx_power = 63;
	}
	cfg.connect_latency = Math.round((parseFloat($("cfg_con_lat").value) / 20.0) - 1);
	if(cfg.connect_latency < 0) cfg.connect_latency = 0;
	else {
	  if(cfg.ver > 0x40)
		  if(cfg.connect_latency > 49) cfg.connect_latency = 49;
	  else
		  if(cfg.connect_latency > 200) cfg.connect_latency = 200;
	}
	if(cfg.ver < 0x20) {
		let s = 'New custom config: ['+cfg.flg+', '+cfg.flg2+', '+cfg.temp_offset+', '+cfg.humi_offset+', '+cfg.advertising_interval+', '+cfg.measure_interval+', '+cfg.rf_tx_power+', '+cfg.connect_latency+', '+cfg.lcd_tint+']';
		addAlog(s);
		sendCustomSetting('55'+hex(cfg.flg,2)+hex(cfg.flg2,2)+hex(cfg.temp_offset&0xff,2)+hex(cfg.humi_offset&0xff,2)+hex(cfg.advertising_interval,2)+hex(cfg.measure_interval,2)+hex(cfg.rf_tx_power,2)+hex(cfg.connect_latency,2)+hex(cfg.lcd_tint,2));
	} else {
		let s = 'New custom config: ['+cfg.flg+', '+cfg.flg2+', '+cfg.temp_offset+', '+cfg.humi_offset+', '+cfg.advertising_interval+', '+cfg.measure_interval+', '+cfg.rf_tx_power+', '+cfg.connect_latency+', '+cfg.lcd_tint+', '+cfg.hver+', '+cfg.av_meas_mem+']';
		addAlog(s);
		sendCustomSetting('55'+hex(cfg.flg,2)+hex(cfg.flg2,2)+hex(cfg.temp_offset&0xff,2)+hex(cfg.humi_offset&0xff,2)+hex(cfg.advertising_interval,2)+hex(cfg.measure_interval,2)+hex(cfg.rf_tx_power,2)+hex(cfg.connect_latency,2)+hex(cfg.lcd_tint,2)+hex(cfg.hver,2)+hex(cfg.av_meas_mem,2));
	}
	$("custcfg").innerHTML = '<hr><button type="button" onclick="sendCustomSetting(&quot;55&quot;);">Get Config</button><br>Send commands to custom firmware:<br><input type="text" id="cmdTXT" value=""><button type="button" onclick="sendCustomSetting($(&quot;cmdTXT&quot;).value,settingsCharacteristics);">Send</button><br>';
}