FROM python:3.9-slim

# 1. Creem un usuari sense privilegis (li diem 'botuser')
RUN useradd -m -u 1000 botuser

WORKDIR /app

RUN pip install flask pandas requests waitress

COPY . /app

# 2. Donem permisos a l'usuari sobre la carpeta de l'app
RUN chown -R botuser:botuser /app

# 3. A PARTIR D'AQUÍ, TOT EL QUE S'EXECUTA HO FA L'USUARI LIMITAT
USER botuser

CMD ["python", "app.py"]
