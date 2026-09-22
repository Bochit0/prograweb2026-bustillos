# Tutorial Completo de Python Básico

## ¿Qué es Python?
Python es un lenguaje de programación de alto nivel, interpretado y multiparadigma. Es ampliamente conocido por su sintaxis simple, limpia y legible, lo que lo hace ideal tanto para principiantes como para el desarrollo de sistemas complejos en backend, automatización y ciencia de datos.

## ¿Cómo arrancar Python?
Para ejecutar código Python, existen dos formas principales desde la terminal:

* **Modo Interactivo (REPL):** Escribe `python3` o `python` en tu terminal. Esto abrirá una consola interactiva (caracterizada por `>>>`) donde puedes probar código en tiempo real.
* **Ejecutar un script:** Crea un archivo con extensión `.py` (por ejemplo, `app.py`). Luego ejecuta en tu terminal: `python3 app.py`.

## Entornos de Desarrollo
* **Editores e IDEs:** Herramientas como Visual Studio Code o PyCharm.
* **Entornos Virtuales (venv):** Herramienta fundamental para aislar las librerías de cada proyecto y evitar conflictos globales.

---

## Tipos de Datos Principales
Python cuenta con diferentes tipos de datos incorporados listos para usar:

* **Enteros y Flotantes (int, float):** `edad = 25`, `precio = 19.99`
* **Cadenas de texto (str):** `nombre = "Steven"`
* **Booleanos (bool):** `es_mayor = True`, `tiene_descuento = False`
* **Listas (list):** Colecciones ordenadas y modificables. `frutas = ["manzana", "pera", "uva"]`
* **Tuplas (tuple):** Colecciones ordenadas pero inmutables (no se pueden cambiar). `coordenadas = (10.5, 20.3)`
* **Diccionarios (dict):** Colecciones de pares clave-valor. `usuario = {"nombre": "Steven", "rol": "admin"}`

## Variables Dinámicas
Python tiene un sistema de **tipado dinámico**. El tipo de dato se asigna automáticamente al darle un valor y puede cambiar durante la ejecución.

```python
mi_variable = 10          # Es un int
mi_variable = "Hola"      # Ahora es un str
```

---

## Estructuras de Control (Condicionales)
**Nota importante:** Python usa la indentación (espacios a la izquierda) para definir bloques de código en lugar de usar llaves `{}`.

```python
edad = 18

if edad >= 18:
    print("Eres mayor de edad.")
elif edad == 17:
    print("Te falta un año.")
else:
    print("Eres menor de edad.")
```

## Bucles (Ciclos)
Los bucles permiten repetir bloques de código.

### 1. Bucle For
Se utiliza para iterar sobre una secuencia (como una lista, un texto o un rango de números).

```python
nombres = ["Steven", "Ana", "Carlos"]
for nombre in nombres:
    print("Hola " + nombre)

# Iterar un número específico de veces
for i in range(3):
    print("Iteración número", i)
```

### 2. Bucle While
Se repite mientras una condición siga siendo verdadera (`True`).

```python
contador = 0
while contador < 3:
    print(contador)
    contador += 1  # Incrementamos en 1 para evitar un bucle infinito
```

---

## Funciones
Las funciones agrupan código para que pueda ser reutilizado. Se definen con la palabra reservada `def`.

```python
def sumar(a, b):
    resultado = a + b
    return resultado

suma_total = sumar(5, 10)
print("La suma es:", suma_total) # Imprime 15
```

## Manejo de Errores (Excepciones)
Se utiliza para evitar que el programa se detenga abruptamente cuando ocurre un fallo.

```python
try:
    resultado = 10 / 0
except ZeroDivisionError:
    print("Error: No se puede dividir por cero.")
finally:
    print("Esta línea siempre se ejecutará al final.")
```

---

## ¿Cómo importar librerías?
Para usar funciones avanzadas o herramientas externas, debes importarlas al inicio de tu script.

### Importación básica
```python
import math
print(math.sqrt(25)) # Uso: modulo.funcion()
```

### Importar algo en específico
```python
from datetime import datetime
print(datetime.now()) # Uso directo sin usar el nombre del módulo antes
```