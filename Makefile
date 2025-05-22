CXX = g++
CXXFLAGS = -Wall -g -std=c++17
SYSTEMC_INC ?= /opt/systemc/include
SYSTEMC_LIB ?= /opt/systemc/lib

INCLUDES = -I$(SYSTEMC_INC)
LDFLAGS = -L$(SYSTEMC_LIB)
LIBS = -lsystemc -lpthread

SRCS = src/main.cpp

BUILD_DIR = build
OBJS = $(SRCS:src/%.cpp=$(BUILD_DIR)/%.o)
TARGET = sim

$(BUILD_DIR)/$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
	rm -f irc_iep_flow.vcd

.PHONY: clean 