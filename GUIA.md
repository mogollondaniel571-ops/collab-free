# Collab Free: guía paso a paso

Son 4 pasos. Los pasos 1 a 3 los haces **una sola vez**. Tus amigos solo hacen el paso 4.

---

## Paso 1: Subir el proyecto a GitHub y compilar el mod

1. Entra a https://github.com y arriba a la derecha toca **+ → New repository**.
2. Ponle de nombre `collab-free`, déjalo en **Public** y toca **Create repository**.
   (Si es público, GitHub compila gratis sin límite.)
3. En la página del repositorio vacío toca el enlace **uploading an existing file**.
4. Descomprime el zip que te pasé. Abre la carpeta `collab-free` y **arrastra todo lo que hay adentro**
   (las carpetas `.github`, `mod`, `server` y los archivos sueltos) a la página de GitHub.
   - Ojo: la carpeta `.github` es la que le dice a GitHub que compile. Si no la ves en Windows,
     en el Explorador activa **Vista → Mostrar → Elementos ocultos**.
5. Abajo toca **Commit changes**.
6. Arriba entra a la pestaña **Actions**. Vas a ver "Compilar mod" corriendo (círculo amarillo).
   Tarda entre 5 y 15 minutos.
7. Cuando salga el check verde ✅, entra a esa ejecución y abajo, en **Artifacts**, descarga
   **collab-free-mod**. Es un zip; adentro está **`daniel.collab-free.geode`**. Ese es el mod.

> Si sale una ❌ roja: entra a la ejecución, abre el paso que falló, copia el error y pásamelo.

---

## Paso 2: Crear el servidor gratis (Render)

1. Entra a https://render.com y crea una cuenta con tu GitHub (botón **GitHub**).
2. Toca **+ New → Web Service** y elige tu repositorio `collab-free`.
3. Llena así:
   - **Root Directory:** `server`
   - **Runtime / Language:** `Node`
   - **Build Command:** `npm install`
   - **Start Command:** `node server.js`
   - **Instance Type:** **Free**
4. Toca **Create Web Service** / **Deploy**. En 1 o 2 minutos dice **Live**.
5. Arriba sale tu dirección, algo como `https://collab-free-xxxx.onrender.com`. **Cópiala.**
6. Para probarla, ábrela en el navegador: tiene que salir algo como `{"ok":true,"name":"Collab Free",...}`.

> El plan gratis se "duerme" si nadie lo usa en 15 minutos. La primera conexión después
> tarda como 1 minuto en despertar. Después va normal.

---

## Paso 3: Instalar Geode v6 (versión de prueba)

El mod usa **Geode v6.0.0-alpha.2**, que es la que funciona con Geometry Dash 2.2091.

1. Entra a https://github.com/geode-sdk/geode/releases
2. Busca **v6.0.0-alpha.2** y descarga lo de Windows (el instalador o el zip de Windows).
3. Instálalo en tu carpeta de Geometry Dash.

> Es una alfa: algunos otros mods tuyos pueden no funcionar todavía con ella.

---

## Paso 4: Instalar el mod (tú y cada amigo)

1. Copia `daniel.collab-free.geode` a la carpeta de mods de Geode:
   en GD abre **Geode → ícono de carpeta**, o ve a la carpeta del juego → `geode/mods`.
2. Abre GD → **Geode → Collab Free → ⚙ ajustes** y en **Dirección del servidor** pega
   la dirección de Render del paso 2. **Todos deben poner la misma.**
3. Inicia sesión en tu cuenta de GD (Ajustes → Cuenta). Sin cuenta no funciona.

---

## Cómo se usa

**Para invitar (anfitrión):**
- En **Mis niveles**, abre tu nivel y toca el botón **Collab** (arriba a la derecha).
  Se abre el editor, se crea la sala y aparece la ventana para invitar.
- O dentro del editor, toca el ícono de amigos (arriba a la derecha) → **Crear sala**.
- En la lista salen tus amigos de GD. Los que dicen **"con el mod abierto"** pueden recibir la
  invitación. También puedes escribir el nombre de usuario e invitar.

**Para unirte:**
- Te sale una ventana: "X te invitó a editar ...". Toca **Unirme**.
  Si estás jugando o en el editor, la invitación aparece cuando sales.
- Se crea una copia del nivel en tus niveles llamada **"Collab ..."** y se abre el editor.

**Mientras editan:**
- Ves la flecha de cada uno con su nombre y color.
- El ícono de Collab se pone **verde** cuando estás en una sala.
- Si el anfitrión se sale, los demás siguen editando (otro pasa a ser anfitrión).
- Si pruebas el nivel (playtest), los cambios de los demás se aplican al terminar de probar.
- Cada uno guarda su propia copia del nivel al salir.

---

## Qué sincroniza y qué no (versión 1)

✅ Poner, borrar, mover, rotar, escalar, cambiar colores de objetos, grupos, configuración de
triggers y cualquier cosa que cambie un objeto.

❌ Los **ajustes del nivel** (canción, colores de fondo/suelo del menú de ajustes, velocidad
inicial) solo se copian al entrar. Si alguien los cambia durante la sesión, los demás no lo ven.

Si dos personas cambian el **mismo objeto** al mismo tiempo, se queda el último cambio que
llegó al servidor.
