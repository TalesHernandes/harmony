# The harmony server: one static binary, no libc, no shell.
#
# The SQLite driver is modernc.org/sqlite — SQLite transpiled to Go — so
# CGO_ENABLED=0 still gives a real SQLite file and the image can stay `scratch`.
# That is the whole reason for the rule in CLAUDE.md: a cgo driver would drag in
# a C toolchain and a base image with a package manager in it.
FROM golang:1.27.3-alpine AS build
# The certificate authorities, for the one thing this server does *outbound*.
#
# Until D4 harmony only ever answered connections; the push to a browser's push
# service is the first one it makes. A `scratch` image has no file at all beyond
# the binary, including no list of authorities, and Go then refuses every TLS
# handshake with "certificate signed by unknown authority" — which is exactly
# what every notification did on the night of 10/09/2026, silently, in the log.
RUN apk add --no-cache ca-certificates
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY cmd ./cmd
COPY internal ./internal
ARG TARGETOS
ARG TARGETARCH
RUN CGO_ENABLED=0 GOOS=${TARGETOS:-linux} GOARCH=${TARGETARCH:-amd64} \
    go build -trimpath -ldflags="-s -w" -o /out/harmony ./cmd/harmony

FROM scratch
COPY --from=build /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
COPY --from=build /out/harmony /harmony
EXPOSE 8080
# The media server's one UDP port. Voice is the only thing in harmony that
# needs a port of its own on the machine; everything else is behind Caddy.
EXPOSE 40000/udp
ENTRYPOINT ["/harmony"]
