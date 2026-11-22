FROM python:3.12-alpine
LABEL org.opencontainers.image.source="https://github.com/synchleague/SliceItAll"
LABEL org.opencontainers.image.description="Slice it All dashboard"
RUN apk add --no-cache openssl ffmpeg
WORKDIR /app
COPY app.py anycubic.py index.html ./
ENV DASHBOARD_PORT=8080
EXPOSE 8080/tcp
EXPOSE 8766/udp
CMD ["python", "-u", "app.py"]
