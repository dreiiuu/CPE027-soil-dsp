.PHONY: test unit sim synth crosscheck firmware-check app-test app-ui dashboard clean

PY ?= python3
CXX ?= g++
STUBS = tests/arduino_stubs
FW = firmware/soil_dsp

test: unit sim synth crosscheck firmware-check app-test
	@echo "ALL CHECKS PASSED"

build:
	mkdir -p build

unit: build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -I $(FW) tests/test_dsp.cpp -o build/test_dsp
	./build/test_dsp

sim: build
	$(CXX) -std=gnu++17 -O1 -Wall -include Arduino.h -I $(STUBS) -I $(FW) tests/host_sim.cpp -o build/host_sim
	./build/host_sim > build/host_sim.out; rc=$$?; grep -E 'FAIL|passed' build/host_sim.out; exit $$rc

firmware-check:
	$(CXX) -std=gnu++17 -Wall -Wextra -fsyntax-only -x c++ -include Arduino.h -I $(STUBS) -I $(FW) $(FW)/soil_dsp.ino

synth: build
	$(PY) analysis/make_synthetic.py --out data/synthetic
	$(PY) analysis/analyze_log.py calib  --air data/synthetic/synthetic_calib_air.csv --water data/synthetic/synthetic_calib_water.csv --out data/synthetic/calibration.json > /dev/null
	$(PY) analysis/analyze_log.py steady --log data/synthetic/synthetic_steady.csv --cal data/synthetic/calibration.json --out build/synth_results > /dev/null
	$(PY) analysis/analyze_log.py step   --log data/synthetic/synthetic_step.csv   --cal data/synthetic/calibration.json --t-step 300 --out build/synth_results > /dev/null
	$(PY) analysis/analyze_log.py pump   --log data/synthetic/synthetic_pump.csv   --cal data/synthetic/calibration.json --out build/synth_results > /dev/null
	@echo "synthetic pipeline OK (simulated data, not results)"

crosscheck: build
	$(PY) analysis/crosscheck.py --log data/synthetic/synthetic_steady.csv --cal data/synthetic/calibration.json
	$(PY) analysis/crosscheck.py --log data/synthetic/synthetic_pump.csv   --cal data/synthetic/calibration.json

# Smart-irrigation firmware run on the PC (real .ino, stubbed hardware) -> its serial output -> PC app tests
app-test: build
	$(CXX) -std=gnu++17 -O1 -Wall -Wextra -include Arduino.h -I $(STUBS) tests/fw_host_run.cpp -o build/fw_host_run
	./build/fw_host_run > build/fw_serial.txt
	$(PY) tests/test_irrigation_ui.py build/fw_serial.txt

# Optional (needs node + playwright): the PC app against its simulator, in Chromium
app-ui: build
	cd software && $(PY) -m irrigation_ui --sim --sim-verified --speed 3 --http-port 8766 --no-browser --log-dir ../build/applogs > ../build/app.log 2>&1 & echo $$! > build/app.pid; sleep 2; \
	node tests/app_check.js http://127.0.0.1:8766/ build; rc=$$?; kill `cat build/app.pid`; exit $$rc

# Optional (needs node + playwright): serve the real dashboard page with a replayed log and check it in Chromium
dashboard: build
	$(PY) tests/dashboard_mock.py --port 8099 > build/mock.log 2>&1 & echo $$! > build/mock.pid; sleep 2; \
	node tests/dashboard_check.js http://localhost:8099/ build; rc=$$?; kill `cat build/mock.pid`; exit $$rc

clean:
	rm -rf build results
