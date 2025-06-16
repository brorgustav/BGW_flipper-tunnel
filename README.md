# BGW_flipper_tunnel
flipperzero-firmware/
└── applications_user/
    └── bgw_flipper_tunnel/
        ├── bgw_flipper_tunnel.c
        └── appinfo.json
MacOSx:

echo "# Setup Flipper dev environment
flipdev() {
  local DEVDIR="/Users/bgw-developer/Library/Mobile Documents/com~apple~CloudDocs/#BGW_flipper-zero/flipper-appdev"
  local FWDIR="$DEVDIR/unleashed-firmware"
  local APPDIR="$FW/user_applications"
  local APPID="bgw_flipper_tunnel"

  source "$DEVDIR/bin/activate" || { echo "❌ Failed to activate venv"
  echo "
  "; return 1; }

  cd "$DEVDIR" || { echo "❌ Failed to cd to developer folder"  echo " 
   "; return 1; }

  echo "✅ Activated Flipper dev environment"
    echo " 
   "
  echo "Developer working directory: $DEVDIR"
    echo " 
   "
  echo "Firmware directory: $FWDIR"
    echo " 
   "
  echo "App directory: $APPDIR"
    echo " 
   "
  echo "App ID: $APPID"
    echo " 
   "
}" >> .bash_profile

#######   RUN IN TERMINAL   ###########
cd
brew install python3
python3 -m venv flipdev
cd flipdev
source bin/activate
# flipdev
pip3 install fbt
git clone https://github.com/DarkFlippers/unleashed-firmware.git
cd unleashed-firmware
cp bgw_flipper_tunnel user_applications/bgw_flipper_tunnel
./fbt launch APPSRC=user_applications/bgw_flipper_tunnel


fbt fap_${APPID}

#fbt --version
#fbt
#place the application folder in flipperzero-firmware/applications_user/




To build your app, run ./fbt fap_{APPID}, where APPID is your app's ID in its manifest.
To build your app and upload it over USB to run on Flipper, use ./fbt launch APPSRC=applications_user/path/to/app. This command is configured in the default VS Code profile as a "Launch App on Flipper" build action (Ctrl+Shift+B menu).
To build an app without uploading it to Flipper, use ./fbt build APPSRC=applications_user/path/to/app. This command is also available in VSCode configuration as "Build App".
To build all FAPs, run ./fbt faps or ./fbt fap_dist.





cd /path/to/unleashed-firmware
ls /dev/cu.*usbmodem*       
fbt flash --interface=/dev/cu.usbmodemflip_Bgwflip1

        fbt flash