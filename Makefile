CXX = g++
CXXFLAGS = -Wall -g -std=c++17
SYSTEMC_INC ?= /opt/systemc/include
SYSTEMC_LIB ?= /opt/systemc/lib
# default pre-processor flags (include dirs)
CPPFLAGS = -I$(SYSTEMC_INC)

LDFLAGS = -L$(SYSTEMC_LIB)
LIBS = -lsystemc -lpthread

SRCS = src/main.cpp
SRCS += src/modules.cpp

BUILD_DIR = build
OBJS = $(SRCS:src/%.cpp=$(BUILD_DIR)/%.o)
TARGET = sim

$(BUILD_DIR)/$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(BUILD_DIR)
	@mkdir -p module_traces
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -rf src/*.o build
	rm -rf module_traces
	rm -f logs
	rm -f *.vcd
	rm -f sim*.txt
	rm -f fifo_sweep_report.csv
	rm -f auto_run.log
	rm -f perf.txt
	rm -f noc_sweep_report.csv

.PHONY: clean 