FROM gcc:12-bookworm AS build
WORKDIR /src
COPY src/ ./src/
RUN gcc -O2 -static -Wall -Wextra -Werror src/*.c -lm -o /bot

FROM scratch
COPY --from=build /bot /bot
USER 65534:65534
ENTRYPOINT ["/bot"]
