FROM python:3.12.10-slim-bookworm
WORKDIR /app
COPY minivectordb.py minivector_sql.py minivector_store.py ./
# Optional acceleration: with numpy present, exact TopK becomes one BLAS
# mat-vec. The install is best-effort on purpose - minivectordb.py falls back to
# the pure-Python distance path, so a build without PyPI access stays correct.
# pip's own --timeout/--retries bound the network wait instead of relying on
# coreutils' timeout being present in the base image.
RUN python -m pip install --no-cache-dir --disable-pip-version-check \
        --timeout 30 --retries 1 "numpy==2.3.5" \
    || echo "numpy unavailable; pure-Python distance path active"
# One BLAS thread: the mat-vec is small, and an unconstrained OpenBLAS would
# spawn threads up to the host CPU count, which risks the container pid limit.
ENV PYTHONDONTWRITEBYTECODE=1 \
    OPENBLAS_NUM_THREADS=1 \
    OMP_NUM_THREADS=1 \
    MKL_NUM_THREADS=1
RUN python -m py_compile minivectordb.py minivector_sql.py minivector_store.py
VOLUME ["/data"]
ENTRYPOINT ["python", "/app/minivectordb.py"]
