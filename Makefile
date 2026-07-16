.PHONY: fmt fmt-check lint quality

fmt:
	./scripts/quality.sh format

fmt-check:
	./scripts/quality.sh format --check

lint:
	./scripts/quality.sh lint

quality: fmt lint
