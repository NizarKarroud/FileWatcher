
FROM python:3.12-slim

WORKDIR /app

COPY server/requirements.txt .

RUN pip install --no-cache-dir -r requirements.txt

COPY server/main.py /app/main.py
COPY generated/ /app/generated/

ENV PYTHONPATH=/app/generated
ENV PYTHONUNBUFFERED=1

EXPOSE 50051

CMD ["python", "main.py"]
