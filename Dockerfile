FROM python:3.12.10-slim-bookworm
WORKDIR /app
# The database itself is a standalone C++17 binary. Python remains in the base
# only because the repository's protocol checker is Python; it is not used by
# the database entrypoint.
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ \
    && rm -rf /var/lib/apt/lists/*
COPY minivectordb.cpp ./
RUN g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra -o minivectordb minivectordb.cpp
VOLUME ["/data"]
ENTRYPOINT ["/app/minivectordb", "--data-dir", "/data"]
