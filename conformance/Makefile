# conformance — goldens (see capture/run_all.sh, goldens/README.md)
.PHONY: goldens goldens-live clean-goldens venv

# Regenerate every golden vector from a local temporal dev server (offline).
goldens:
	./capture/run_all.sh

# ...plus the read-only live smoke against api.mistral.ai (needs MISTRAL_API_KEY).
goldens-live:
	./capture/run_all.sh --live

# Provision the repo-local venv without capturing.
venv:
	python3 -m venv .venv && ./.venv/bin/pip install -q --upgrade pip && ./.venv/bin/pip install -e .

clean-goldens:
	rm -f goldens/histories/*.json goldens/payloads/*.json
