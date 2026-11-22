# apex-mech

Autonomous Mech Royale bot for the Builders Buildathon (`mechfight/1` protocol), written in C with zero external dependencies and packaged as a static binary in a `FROM scratch` Docker image.

## Build

```sh
docker build -t apex-mech .
```

Or compile locally:

```sh
gcc -O2 -static -Wall -Wextra -Werror src/*.c -lm -o bot
```
