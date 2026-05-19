CXX := g++
CXXFLAGS := -std=c++20 -O2
INCLUDES := -I. -Irepos/llama.cpp/include -Irepos/llama.cpp/ggml/include
LDLIBS := -lpthread -lm -ldl

SRCDIR := src
OBJDIR := build/obj
TARGET := mdmem

# Discover source files
SRCS := $(wildcard $(SRCDIR)/*.cpp)
OBJS := $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SRCS))

# Discover llama.cpp static libraries dynamically
LLAMA_LIBS := $(shell find repos/llama.cpp/build -name '*.a')

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ $(LLAMA_LIBS) $(LDLIBS) -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

.PHONY: clean
clean:
	rm -rf $(OBJDIR) $(TARGET)
