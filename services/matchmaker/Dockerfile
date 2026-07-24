# syntax=docker/dockerfile:1

# ---- build stage: static, stripped binary -----------------------------------
FROM golang:1.26-alpine AS build
WORKDIR /src

# Cache modules first for faster rebuilds.
COPY go.mod go.sum ./
RUN go mod download

COPY . .
# CGO off → a fully static binary that runs in a scratch/distroless image.
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /out/matchmaker .

# ---- run stage: minimal, non-root -------------------------------------------
FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /out/matchmaker /matchmaker

EXPOSE 8080/tcp
EXPOSE 8081/udp
EXPOSE 8082/udp

USER nonroot:nonroot
ENTRYPOINT ["/matchmaker"]
