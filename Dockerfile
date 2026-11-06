# Builds the WebAssembly game into one self-contained index.html and serves it with nginx.
#   docker build -t mi-demo .   &&   docker run --rm -p 8080:80 mi-demo   ->  http://localhost:8080
# Needs the demo's data files in ./mi_demo2 (DISK01.LEC, 000.LFL, 901/902/904.LFL); they are embedded into the page.

FROM debian:trixie-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends emscripten python3 ca-certificates \
 && rm -rf /var/lib/apt/lists/*
ENV EM_CACHE=/tmp/emscripten-cache
WORKDIR /src
COPY engine ./engine
COPY web ./web
COPY mi_demo2 ./mi_demo2
RUN cd web && ./build.sh

FROM nginx:alpine
COPY --from=build /src/web/dist/index.html /usr/share/nginx/html/index.html
EXPOSE 80
