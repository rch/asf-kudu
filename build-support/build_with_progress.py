#!/usr/bin/env python3
"""
Wrapper for make that displays a rich progress bar during compilation.

Parses make output for percentage indicators like "[ 98%]" and displays
a visual progress bar using the rich library.

Usage:
    python3 build_with_progress.py make -j$(nproc)
"""

import re
import subprocess
import sys
from rich.console import Console
from rich.progress import Progress, SpinnerColumn, BarColumn, TextColumn, TimeRemainingColumn

def main():
    if len(sys.argv) < 2:
        print("Usage: build_with_progress.py <command> [args...]", file=sys.stderr)
        sys.exit(1)

    command = sys.argv[1:]

    # Regex to match progress indicators like "[ 98%]" or "[100%]"
    progress_pattern = re.compile(r'\[\s*(\d+)%\]')

    # Regex to extract target names from lines like "[ 98%] Building CXX object ..."
    target_pattern = re.compile(r'\[\s*\d+%\]\s+(Building|Linking|Generating|Creating)\s+(\S+)\s+(.+)')

    console = Console()

    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        BarColumn(),
        TextColumn("[progress.percentage]{task.percentage:>3.0f}%"),
        TimeRemainingColumn(),
        console=console,
    ) as progress:

        task = progress.add_task("[cyan]Building Kudu", total=100)
        current_target = ""
        last_percentage = 0

        try:
            # Run the command and capture output line by line
            process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                universal_newlines=True,
                bufsize=1  # Line buffered
            )

            for line in process.stdout:
                # Check for progress percentage
                match = progress_pattern.search(line)
                if match:
                    percentage = int(match.group(1))
                    if percentage != last_percentage:
                        progress.update(task, completed=percentage)
                        last_percentage = percentage

                # Extract target being built
                target_match = target_pattern.search(line)
                if target_match:
                    action = target_match.group(1)
                    obj_type = target_match.group(2)
                    target_name = target_match.group(3).strip()

                    # Shorten long paths for display
                    if len(target_name) > 50:
                        target_name = "..." + target_name[-47:]

                    current_target = f"{action} {obj_type}: {target_name}"
                    progress.update(task, description=f"[cyan]{current_target}")

                # Print the line (rich will handle positioning)
                # Only print non-progress lines to avoid clutter
                if not progress_pattern.search(line) and not line.strip().startswith("Built target"):
                    # Print important lines only
                    if any(keyword in line for keyword in ["Error", "error:", "Warning", "warning:", "FAILED"]):
                        console.print(line, end='', style="bold red" if "error" in line.lower() else "yellow")

            process.wait()

            # Update to 100% when done
            if process.returncode == 0:
                progress.update(task, completed=100, description="[green]Build completed successfully!")
            else:
                progress.update(task, description=f"[red]Build failed with exit code {process.returncode}")

            return process.returncode

        except KeyboardInterrupt:
            console.print("\n[yellow]Build interrupted by user[/yellow]")
            process.terminate()
            return 130  # Standard exit code for SIGINT
        except Exception as e:
            console.print(f"\n[red]Error running build: {e}[/red]")
            return 1

if __name__ == "__main__":
    sys.exit(main())
