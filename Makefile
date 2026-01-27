# Makefile para NVDAAL.kext
# Driver NVIDIA Ada Lovelace para macOS Hackintosh

KEXT_NAME = NVDAAL
BUNDLE_ID = com.nvidia.nvdaal
BUILD_DIR = Build
KEXT_PATH = $(BUILD_DIR)/$(KEXT_NAME).kext
INFO_PLIST = Info.plist
SOURCE_FILE = Sources/NVDAAL.cpp

# SDK e ferramentas
SDKROOT = $(shell xcrun --show-sdk-path)
CXX = clang++
CXXFLAGS = -arch x86_64 \
         -mmacosx-version-min=13.0 \
         -mkernel \
         -nostdinc \
         -fno-builtin \
         -fno-common \
         -fno-exceptions \
         -fno-rtti \
         -O2 \
         -DKERNEL \
         -DKERNEL_PRIVATE \
         -DDRIVER_PRIVATE \
         -DAPPLE \
         -DNeXT

INCLUDES = -I$(SDKROOT)/System/Library/Frameworks/Kernel.framework/Headers \
           -I$(SDKROOT)/System/Library/Frameworks/IOKit.framework/Headers

LDFLAGS = -arch x86_64 \
          -static \
          -nostdlib \
          -Wl,-kext \
          -Wl,-exported_symbols_list,/dev/null \
          -lkmod

all: $(KEXT_PATH)

$(KEXT_PATH): $(BUILD_DIR) $(KEXT_PATH)/Contents/MacOS/$(KEXT_NAME) $(KEXT_PATH)/Contents/Info.plist

$(KEXT_PATH)/Contents/MacOS/$(KEXT_NAME): $(SOURCE_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ $<

$(KEXT_PATH)/Contents/Info.plist: $(INFO_PLIST)
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
	@echo "Build limpo."

sign:
	@echo "[!] Assinatura não necessária em Hackintosh (use kext-dev-mode=1 ou amfi_get_out_of_my_way=0x1)."
	@# codesign -s - $(KEXT_PATH) --deep --force

install: $(KEXT_PATH)
	@echo "[*] Instalando NVDAAL.kext..."
	sudo chown -R root:wheel $(KEXT_PATH)
	sudo cp -R $(KEXT_PATH) /Library/Extensions/
	sudo touch /Library/Extensions/
	sudo kextcache -invalidate /
	@echo "[+] Kext instalado. Reboot necessário."

load: $(KEXT_PATH)
	@echo "[*] Carregando NVDAAL.kext (temporário)..."
	sudo chown -R root:wheel $(KEXT_PATH)
	sudo kextload $(KEXT_PATH)
	@echo "[+] Kext carregado. Verifique com: log show --predicate 'eventMessage contains \"NVDAAL\"' --last 1m"

unload:
	@echo "[*] Descarregando NVDAAL.kext..."
	sudo kextunload -b $(BUNDLE_ID)
	@echo "[+] Kext descarregado."

test: $(KEXT_PATH)
	@echo "[*] Verificando estrutura do kext..."
	@ls -la $(KEXT_PATH)/Contents/
	@ls -la $(KEXT_PATH)/Contents/MacOS/
	@echo "[*] Validando Info.plist..."
	@plutil $(KEXT_PATH)/Contents/Info.plist
	@echo "[+] Estrutura OK!"

.PHONY: all clean sign install load unload test
