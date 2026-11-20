"""Credential-free failure context: container state and selected service metrics."""
import subprocess
import urllib.request


def main():
    subprocess.run(["docker", "ps", "-a", "--filter", "label=com.docker.compose.project=dynaxis-stack",
                    "--format", "{{.Names}} {{.Status}}"], check=False)
    for port in (36001, 36002, 39091, 39092, 39093):
        for route in ("readyz", "metrics"):
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{port}/{route}", timeout=2) as response:
                    text = response.read().decode()
                    if route == "metrics":
                        text = "\n".join(line for line in text.splitlines() if line.startswith((
                            "runtime_dependency_", "gateway_ingress_", "gateway_backend_", "runtime_lifecycle_")))
                    print(f"{port}/{route}: {text}")
            except Exception as error:
                print(f"{port}/{route}: {type(error).__name__}")


if __name__ == "__main__":
    main()
