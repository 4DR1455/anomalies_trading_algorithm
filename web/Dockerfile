FROM python:3.9-slim

# 1. Creem un usuari sense privilegis per seguretat
RUN useradd -m -u 1001 botuser

WORKDIR /app

# 2. Instal·lem les dependències necessàries per al dashboard unificat
# Afegim flask per al servidor, waitress per a producció i pandas/requests per si de cas
RUN pip install --no-cache-dir flask waitress pandas requests

# 3. Copiem el codi de la web (app.py i la carpeta templates)
COPY . /app

# 4. Donem permisos a l'usuari sobre la carpeta de l'app
RUN chown -R botuser:botuser /app

# 5. Executem com a usuari limitat
USER botuser

# Exposem el port intern
EXPOSE 5000

# Iniciem el servidor unificat
CMD ["python", "app.py"]
