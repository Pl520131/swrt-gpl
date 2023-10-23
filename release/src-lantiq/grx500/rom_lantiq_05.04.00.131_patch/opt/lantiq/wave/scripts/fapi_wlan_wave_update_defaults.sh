#!/tmp/wireless/lantiq/bin/sh

export PATH=/tmp/wireless/lantiq/bin/:$PATH
if [ "$1" = "" ] || [ "$2" = "" ] || [ "$3" = "" ] || [ "$4" = "" ] || [ "$5" = "" ] || [ "$6" = "" ] || [ "$7" = "" ] || [ "$8" = "" ]; then
	echo "Usage: /opt/lantiq/wave/scripts/update_asus_defaults.sh SEC_24 SEC_5 PWD_24 PWD_5 CHANNEL_24 CHANNEL_5 REG_24 REG_5"
	echo "SSID_X: SSID"
	echo "SEC_X: None,WEP-64,WEP-128,WPA-Personal,WPA2-Personal,WPA-WPA2-Personal,WPA-Enterprise,WPA2-Enterprise,WPA-WPA2-Enterprise"
	echo "PWD_X: Passphrase or WEP kez\nCHANNEL_X: 0: AutoChannel, 1..: Channel"
	echo "Example: /opt/lantiq/wave/scripts/update_asus_defaults.sh test_ssid_wlan0 test_ssid_wlan1 WPA-Personal WPA2-Personal test_passphrase_wlan0 test_passphrase_wlan1 3 44 US US"
	exit
fi

DBDIR_0=/opt/lantiq/wave/db/default/radio0
DBDIR_1=/opt/lantiq/wave/db/default/radio2

DEFFILE_RADIO=Device.WiFi.Radio
DEFFILE_RADIO_VENDOR=Device.WiFi.Radio.X_LANTIQ_COM_Vendor
DEFFILE_SSID=Device.WiFi.SSID
DEFFILE_SECURITY=Device.WiFi.AccessPoint.Security
DEFFILE_WPS=Device.WiFi.AccessPoint.WPS
DEFFILE_WPS_CONFIGSTATE=Device.WiFi.Radio.X_LANTIQ_COM_Vendor.WPS
ssid_24=`nvram get wl0_ssid`
ssid_5=`nvram get wl1_ssid`
security_24=$1
security_5=$2
password_24=$3
password_5=$4
channel_24=$5
channel_5=$6
ccode_24=$7
ccode_5=$8
pincode=`nvram get secret_code`
wifi_psk=`nvram get wifi_psk`

if [ "$wifi_psk" != "" ] ; then
	security_24="WPA2-Personal"
	security_5="WPA2-Personal"
	password_24=`nvram get wl0_wpa_psk`
	password_5=`nvram get wl1_wpa_psk`
fi

cd $DBDIR_0
sed -i "s/SSID_0=test_ssid/SSID_0=$ssid_24/g" $DEFFILE_SSID
sed -i "s/ModeEnabled_0=WPA2-Personal/ModeEnabled_0=$security_24/g" $DEFFILE_SECURITY
if [ "$security_24" = "WPA2-Personal" ] || [ "$security_24" = "WPA2-Enterprise" ] ; then
	sed -i "s/KeyPassphrase_0=test_passphrase/KeyPassphrase_0=$password_24/g" $DEFFILE_SECURITY
else
	sed -i "s/Enable_0=true/Enable_0=false/g" $DEFFILE_WPS
	if [ "$security_24" = "WEP-64" ] || [ "$security_24" = "WEP-128" ] ; then
		sed -i "s/WEPKey_0=123456789a/WEPKey_0=$password_24/g" $DEFFILE_SECURITY
	else
		sed -i "s/KeyPassphrase_0=test_passphrase/KeyPassphrase_0=$password_24/g" $DEFFILE_SECURITY
	fi
fi

if [ "$channel_24" != "" ] ; then
	if [ "$channel_24" == "0" ] ; then
		sed -i "/AutoChannelEnable_0=/c AutoChannelEnable_0=true" $DEFFILE_RADIO
	else
		sed -i "/\bChannel_0=/c Channel_0=$channel_24" $DEFFILE_RADIO
		sed -i "/AutoChannelEnable_0=/c AutoChannelEnable_0=false" $DEFFILE_RADIO
	fi
	if [ "$channel_24" -lt "5" ] ; then
		sed -i "s/ExtensionChannel_0=BelowControlChannel/ExtensionChannel_0=AboveControlChannel/g" $DEFFILE_RADIO
	fi
fi
if [ "$ccode_24" != "" ] ; then
	sed -i "s/RegulatoryDomain_0=US/RegulatoryDomain_0=$ccode_24/g" $DEFFILE_RADIO
	if [ "$ccode_24" = "GB" ] ; then
		sed -i "s/WaveTxOpMode_0=Dynamic/WaveTxOpMode_0=Disabled/g" $DEFFILE_RADIO_VENDOR
	fi
fi

sed -i "s/OperatingStandards_0=b,g,n/OperatingStandards_0=n,g/g" $DEFFILE_RADIO
sed -i "s/OperatingChannelBandwidth_0=Auto/OperatingChannelBandwidth_0=40MHz/g" $DEFFILE_RADIO
sed -i "s/CoexRssiThreshold_0=-20/CoexRssiThreshold_0=-70/g" $DEFFILE_RADIO_VENDOR

sed -i "s/ConfigState_0=Configured/ConfigState_0=Unconfigured/g" $DEFFILE_WPS_CONFIGSTATE
sed -i "s/WaveFastpathEnabled_0=false/WaveFastpathEnabled_0=true/g" $DEFFILE_RADIO_VENDOR
sed -i "s/^PIN_0=.*/PIN_0=$pincode/g" $DEFFILE_WPS_CONFIGSTATE
cat $DEFFILE_SSID | grep SSID_0
cat $DEFFILE_SECURITY | grep ModeEnabled_0
cat $DEFFILE_SECURITY | grep KeyPassphrase_0
cat $DEFFILE_SECURITY | grep WEPKey_0
cat $DEFFILE_WPS | grep Enable_0
cat $DEFFILE_RADIO | grep Channel_0
cat $DEFFILE_RADIO | grep AutoChannelEnable_0

cd $DBDIR_1
sed -i "s/SSID_0=test_ssid/SSID_0=$ssid_5/g" $DEFFILE_SSID
sed -i "s/ModeEnabled_0=WPA2-Personal/ModeEnabled_0=$security_5/g" $DEFFILE_SECURITY
if [ "$security_5" = "WPA2-Personal" ] || [ "$security_5" = "WPA2-Enterprise" ] ; then
	sed -i "s/KeyPassphrase_0=test_passphrase/KeyPassphrase_0=$password_5/g" $DEFFILE_SECURITY
else
	sed -i "s/Enable_0=true/Enable_0=false/g" $DEFFILE_WPS
	if [ "$security_5" = "WEP-64" ] || [ "$security_5" = "WEP-128" ] ; then
		sed -i "s/WEPKey_0=123456789a/WEPKey_0=$password_5/g" $DEFFILE_SECURITY
	else
		sed -i "s/KeyPassphrase_0=test_passphrase/KeyPassphrase_0=$password_5/g" $DEFFILE_SECURITY
	fi
fi

if [ "$channel_5" != "" ] ; then
	if [ "$channel_5" == "0" ] ; then
		sed -i "/AutoChannelEnable_0=/c AutoChannelEnable_0=true" $DEFFILE_RADIO
	else
		sed -i "/\bChannel_0=/c Channel_0=$channel_5" $DEFFILE_RADIO
		sed -i "/AutoChannelEnable_0=/c AutoChannelEnable_0=false" $DEFFILE_RADIO
	fi
fi
if [ "$ccode_5" != "" ] ; then
	sed -i "s/RegulatoryDomain_0=US/RegulatoryDomain_0=$ccode_5/g" $DEFFILE_RADIO
	if [ "$ccode_5" = "GB" ] ; then
		sed -i "s/IEEE80211hEnabled_0=false/IEEE80211hEnabled_0=true/g" $DEFFILE_RADIO
		sed -i "s/WaveTxOpMode_0=Dynamic/WaveTxOpMode_0=Disabled/g" $DEFFILE_RADIO_VENDOR
	fi
fi

sed -i "s/OperatingStandards_0=a,n,ac/OperatingStandards_0=n,a,ac/g" $DEFFILE_RADIO
sed -i "s/OperatingChannelBandwidth_0=Auto/OperatingChannelBandwidth_0=80MHz/g" $DEFFILE_RADIO
sed -i "s/ConfigState_0=Configured/ConfigState_0=Unconfigured/g" $DEFFILE_WPS_CONFIGSTATE
sed -i "s/^PIN_0=.*/PIN_0=$pincode/g" $DEFFILE_WPS_CONFIGSTATE
cat $DEFFILE_SSID | grep SSID_0
cat $DEFFILE_SECURITY | grep ModeEnabled_0
cat $DEFFILE_SECURITY | grep KeyPassphrase_0
cat $DEFFILE_SECURITY | grep WEPKey_0
cat $DEFFILE_WPS | grep Enable_0
cat $DEFFILE_RADIO | grep Channel_0
cat $DEFFILE_RADIO | grep AutoChannelEnable_0

sed -i "s/WaveExternallyManaged_0=true/WaveExternallyManaged_0=false/g" /opt/lantiq/wave/db/default/radio0/$DEFFILE_RADIO_VENDOR
sed -i "s/WaveExternallyManaged_0=true/WaveExternallyManaged_0=false/g" /opt/lantiq/wave/db/default/radio2/$DEFFILE_RADIO_VENDOR

sed -i "s/WaveIgnore40MhzIntolerant_0=false/WaveIgnore40MhzIntolerant_0=true/g" /opt/lantiq/wave/db/default/radio0/$DEFFILE_RADIO_VENDOR
sed -i "s/WaveIgnore40MhzIntolerant_0=false/WaveIgnore40MhzIntolerant_0=true/g" /opt/lantiq/wave/db/default/radio2/$DEFFILE_RADIO_VENDOR

