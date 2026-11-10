FROM python:3.12.10-slim-bookworm
WORKDIR /app
COPY minivectordb.py minivector_sql.py minivector_store.py ./
RUN python -m py_compile minivectordb.py minivector_sql.py minivector_store.py
ENV PYTHONDONTWRITEBYTECODE=1
VOLUME ["/data"]
ENTRYPOINT ["python", "/app/minivectordb.py"]
