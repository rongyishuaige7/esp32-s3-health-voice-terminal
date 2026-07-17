.PHONY: verify repo-check tests build

verify:
	bash scripts/verify.sh

repo-check:
	python3 scripts/secret_scan.py
	python3 scripts/check_repo.py

tests:
	python3 -m unittest discover -s tests -v

build:
	bash -lc 'source /home/rongyi/.espressif/v5.5/esp-idf/export.sh >/dev/null && idf.py build'
