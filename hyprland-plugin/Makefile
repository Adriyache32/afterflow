CXX      ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++2b -fPIC -shared
PKGS      = hyprland

BUILD_DIR = ../build-hyprplugin
TARGET    = $(BUILD_DIR)/afterflow-capture-exclusion.so
SOURCES   = main.cpp

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES) $(shell pkg-config --cflags --libs $(PKGS))

install: $(TARGET)
	@mkdir -p $(HOME)/.local/lib/afterflow
	install -m 755 $(TARGET) $(HOME)/.local/lib/afterflow/afterflow-capture-exclusion.so

clean:
	rm -rf $(BUILD_DIR)
