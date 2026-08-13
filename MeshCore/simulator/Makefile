CXX ?= clang++
CPPFLAGS := -Ihost -I. -I../src -I../src/helpers -I../lib/ed25519 -MMD -MP \
	-DESP32 -DMESHCORE_SIMULATOR -DMESH_DEBUG=0 -DMESH_PACKET_LOGGING=0 \
	-DMAX_NEIGHBOURS=0 -DMAX_CONTACTS=100 -DMAX_GROUP_CHANNELS=40
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wpedantic -Wno-unused-parameter -Wno-reorder-ctor
LDLIBS := -lsqlite3

BUILD := build
TARGET := $(BUILD)/meshcore-repeater-sim
SOURCES := main.cpp Simulator.cpp SimulatorIdentityRegistry.cpp CompanionEndpoint.cpp TcpSerialInterface.cpp HostPlatform.cpp RepeaterFirmware.cpp \
	../src/Packet.cpp ../src/Dispatcher.cpp ../src/Mesh.cpp ../src/Utils.cpp \
	../src/Identity.cpp ../src/helpers/StaticPoolPacketManager.cpp \
	../src/helpers/AdvertDataHelpers.cpp ../src/helpers/TxtDataHelpers.cpp \
	../src/helpers/ConfigSerializer.cpp ../src/helpers/CommonCLI.cpp \
	../src/helpers/ClientACL.cpp ../src/helpers/RegionMap.cpp \
	../src/helpers/TransportKeyStore.cpp ../src/helpers/IdentityStore.cpp \
	../src/helpers/BaseChatMesh.cpp
OBJECTS := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(SOURCES)))
OBJECTS += $(BUILD)/RepeaterMyMesh.o $(BUILD)/CompanionMyMesh.o $(BUILD)/CompanionDataStore.o \
	$(patsubst ../lib/ed25519/%.c,$(BUILD)/%.o,$(wildcard ../lib/ed25519/*.c))
DEPS := $(OBJECTS:.o=.d)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(OBJECTS) $(LDLIBS) -o $@

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Packet.o: ../src/Packet.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Dispatcher.o: ../src/Dispatcher.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Mesh.o: ../src/Mesh.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Utils.o: ../src/Utils.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Identity.o: ../src/Identity.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: ../src/helpers/%.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/StaticPoolPacketManager.o: ../src/helpers/StaticPoolPacketManager.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/RepeaterMyMesh.o: ../examples/simple_repeater/MyMesh.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I../examples/simple_repeater \
		-DMyMesh=RepeaterFirmwareMyMesh -DNodePrefs=RepeaterFirmwareNodePrefs \
		-Dboard=repeater_firmware_board -Dradio_driver=repeater_firmware_radio_driver \
		-Drtc_clock=repeater_firmware_rtc_clock -Dsensors=repeater_firmware_sensors \
		-Dradio_new_identity=repeater_firmware_new_identity -c $< -o $@

$(BUILD)/CompanionMyMesh.o: ../examples/companion_radio/MyMesh.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I../examples/companion_radio \
		-DMyMesh=CompanionFirmwareMyMesh -DNodePrefs=CompanionFirmwareNodePrefs \
		-Dboard=companion_firmware_board -Dradio_driver=companion_firmware_radio_driver \
		-Drtc_clock=companion_firmware_rtc_clock -Dsensors=companion_firmware_sensors \
		-Dradio_new_identity=companion_firmware_new_identity -c $< -o $@

$(BUILD)/CompanionDataStore.o: ../examples/companion_radio/DataStore.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I../examples/companion_radio \
		-DNodePrefs=CompanionFirmwareNodePrefs -c $< -o $@

$(BUILD)/CommonCLI.o: ../src/helpers/CommonCLI.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DNodePrefs=RepeaterFirmwareNodePrefs -c $< -o $@

$(BUILD)/%.o: ../lib/ed25519/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) -std=c99 -O2 -Wall -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

test: $(TARGET)
	$(TARGET) --self-test

clean:
	rm -rf $(BUILD)

-include $(DEPS)
