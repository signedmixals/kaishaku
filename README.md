# kaishaku v0.9.3 - manage detached HEAD sessions for safe Git experimentation


## Description

kaishaku creates isolated Git sessions where you can:
- Try out risky changes
- Test different solutions
- Make commits
- Switch between experiments

All without touching your working branches.

## Installation

```bash
# Clone the repository
git clone https://github.com/signedmixals/kaishaku.git
cd kaishaku

# Build it
gcc -o kaishaku kaishaku.c -O3
```

## Usage

```bash
# Start experimenting
kaishaku checkout experiment1

# Make changes, commit them...

# Save to a branch if you like the result
kaishaku save feature-branch

# Or try something else
kaishaku checkout experiment2
```

## Examples

### Basic Usage

```bash
# Create a new session from current HEAD
kaishaku checkout feature-test

# Create a session from a specific commit
kaishaku checkout bugfix 1234abc

# Switch to an existing session
kaishaku switch feature-test

# Save changes to a new branch
kaishaku save new-feature

# List all sessions
kaishaku list

# Exit current session
kaishaku exit --save
```

### Advanced Usage

```bash
# Start with a clean state
git checkout main
git pull

# Create multiple sessions for different approaches
kaishaku checkout feature-a
git add . && git commit -m "Initial approach"

kaishaku checkout feature-a-alt
git add . && git commit -m "Alternative approach"

# Switch between approaches
kaishaku switch feature-a
kaishaku switch feature-a-alt

# Compare and decide
kaishaku list

# Save the chosen approach
kaishaku save feature-a-final

# Clean up unused sessions
kaishaku clean feature-a
kaishaku clean feature-a-alt
```

## Features

- No more temporary branches cluttering your repository
- No risk of accidentally committing to wrong branches
- Easy to switch between different approaches
- Simple to recover from mistakes

## Configuration

Configure how kaishaku handles your changes when exiting a session:

```bash
# Automatically save changes when exiting a session
kaishaku config set auto.save 1

# Automatically stash changes when exiting a session
kaishaku config set auto.stash 1

# Disable confirmation prompt when exiting
kaishaku config set confirm.exit 0
```


## License

This project is licensed under the MIT License - see the LICENSE file for details.

