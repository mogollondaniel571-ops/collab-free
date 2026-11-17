// Instalador de Collab Free para Windows.
// Instala Geode v6.0.0-alpha.2 (si falta) y copia los mods que vienen adentro.
package main

import (
	"archive/zip"
	"bufio"
	"bytes"
	"embed"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

//go:embed files/*.geode
var mods embed.FS

const (
	geodeZip     = "https://github.com/geode-sdk/geode/releases/download/v6.0.0-alpha.2/geode-v6.0.0-alpha.2-win.zip"
	resourcesZip = "https://github.com/geode-sdk/geode/releases/download/v6.0.0-alpha.2/resources.zip"
)

// la versión la pone GitHub al compilar (sale de mod/mod.json)
var version = "dev"

var in = bufio.NewReader(os.Stdin)

func pause(msg string) {
	fmt.Print(msg)
	in.ReadString('\n')
}

func fail(msg string, err error) {
	fmt.Println()
	fmt.Println("ERROR:", msg)
	if err != nil {
		fmt.Println("Detalle:", err)
	}
	pause("\nPresiona Enter para cerrar...")
	os.Exit(1)
}

// ---------- permisos de administrador ----------

func isAdmin() bool {
	r, _, _ := syscall.NewLazyDLL("shell32.dll").NewProc("IsUserAnAdmin").Call()
	return r != 0
}

func relaunchAsAdmin() bool {
	exe, err := os.Executable()
	if err != nil {
		return false
	}
	verb, _ := syscall.UTF16PtrFromString("runas")
	file, _ := syscall.UTF16PtrFromString(exe)
	cwd, _ := syscall.UTF16PtrFromString(filepath.Dir(exe))
	r, _, _ := syscall.NewLazyDLL("shell32.dll").NewProc("ShellExecuteW").Call(
		0, uintptr(unsafe.Pointer(verb)), uintptr(unsafe.Pointer(file)), 0,
		uintptr(unsafe.Pointer(cwd)), 1)
	return r > 32
}

// ---------- buscar Geometry Dash ----------

func steamRoots() []string {
	roots := []string{}
	out, err := exec.Command("reg", "query", `HKCU\Software\Valve\Steam`, "/v", "SteamPath").Output()
	if err == nil {
		if m := regexp.MustCompile(`SteamPath\s+REG_SZ\s+(.+)`).FindSubmatch(out); m != nil {
			roots = append(roots, filepath.Clean(strings.TrimSpace(string(m[1]))))
		}
	}
	roots = append(roots, `C:\Program Files (x86)\Steam`, `C:\Program Files\Steam`)
	// otras bibliotecas de Steam (otros discos)
	more := []string{}
	for _, r := range roots {
		data, err := os.ReadFile(filepath.Join(r, "steamapps", "libraryfolders.vdf"))
		if err != nil {
			continue
		}
		for _, m := range regexp.MustCompile(`"path"\s+"([^"]+)"`).FindAllSubmatch(data, -1) {
			more = append(more, strings.ReplaceAll(string(m[1]), `\\`, `\`))
		}
	}
	return append(roots, more...)
}

func findGD() string {
	for _, r := range steamRoots() {
		dir := filepath.Join(r, "steamapps", "common", "Geometry Dash")
		if _, err := os.Stat(filepath.Join(dir, "GeometryDash.exe")); err == nil {
			return dir
		}
	}
	fmt.Println("No encontré Geometry Dash automáticamente.")
	fmt.Println("(En Steam: clic derecho a Geometry Dash > Administrar > Ver archivos locales,")
	fmt.Println(" copia la dirección de arriba y pégala aquí.)")
	for {
		fmt.Print("\nCarpeta de Geometry Dash: ")
		line, _ := in.ReadString('\n')
		dir := strings.Trim(strings.TrimSpace(line), `"`)
		if _, err := os.Stat(filepath.Join(dir, "GeometryDash.exe")); err == nil {
			return dir
		}
		fmt.Println("En esa carpeta no está GeometryDash.exe. Intenta otra vez.")
	}
}

func gdRunning() bool {
	out, _ := exec.Command("tasklist", "/FI", "IMAGENAME eq GeometryDash.exe").Output()
	return bytes.Contains(bytes.ToLower(out), []byte("geometrydash.exe"))
}

// ---------- descargas ----------

func download(url string) ([]byte, error) {
	c := &http.Client{Timeout: 5 * time.Minute}
	resp, err := c.Get(url)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("el servidor respondió %s", resp.Status)
	}
	return io.ReadAll(resp.Body)
}

func unzipTo(data []byte, dest string) error {
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return err
	}
	for _, f := range zr.File {
		if strings.HasSuffix(f.Name, ".pdb") || strings.HasSuffix(f.Name, ".lib") {
			continue // archivos de programador, no hacen falta para jugar
		}
		p := filepath.Join(dest, f.Name)
		if !strings.HasPrefix(p, filepath.Clean(dest)+string(os.PathSeparator)) {
			continue
		}
		if f.FileInfo().IsDir() {
			os.MkdirAll(p, 0755)
			continue
		}
		os.MkdirAll(filepath.Dir(p), 0755)
		rc, err := f.Open()
		if err != nil {
			return err
		}
		out, err := os.Create(p)
		if err != nil {
			rc.Close()
			return err
		}
		_, err = io.Copy(out, rc)
		out.Close()
		rc.Close()
		if err != nil {
			return err
		}
	}
	return nil
}

func main() {
	fmt.Println("==============================================")
	fmt.Println("   Instalador de Collab Free v" + version)
	fmt.Println("==============================================")
	fmt.Println()

	if !isAdmin() {
		fmt.Println("Pidiendo permisos de administrador...")
		if relaunchAsAdmin() {
			return
		}
		fail("No se pudieron obtener permisos de administrador.\n"+
			"Clic derecho al instalador > Ejecutar como administrador.", nil)
	}

	gd := findGD()
	fmt.Println("Geometry Dash encontrado en:", gd)

	for gdRunning() {
		pause("\nGeometry Dash está abierto. Ciérralo y presiona Enter...")
	}

	// Geode
	if _, err := os.Stat(filepath.Join(gd, "Geode.dll")); err == nil {
		fmt.Println("Geode ya está instalado (se actualizará solo si hace falta).")
	} else {
		fmt.Println("\nDescargando Geode v6.0.0-alpha.2...")
		data, err := download(geodeZip)
		if err != nil {
			fail("No se pudo descargar Geode. Revisa tu internet.", err)
		}
		if err := unzipTo(data, gd); err != nil {
			fail("No se pudo instalar Geode.", err)
		}
		fmt.Println("Descargando recursos de Geode...")
		if res, err := download(resourcesZip); err == nil {
			unzipTo(res, filepath.Join(gd, "geode", "resources", "geode.loader"))
		} else {
			fmt.Println("(No se pudieron bajar los recursos; Geode los descarga solo al abrir el juego.)")
		}
		fmt.Println("Geode instalado.")
	}

	// Mods
	modsDir := filepath.Join(gd, "geode", "mods")
	if err := os.MkdirAll(modsDir, 0755); err != nil {
		fail("No se pudo crear la carpeta de mods.", err)
	}
	entries, _ := mods.ReadDir("files")
	for _, e := range entries {
		data, _ := mods.ReadFile("files/" + e.Name())
		if err := os.WriteFile(filepath.Join(modsDir, e.Name()), data, 0644); err != nil {
			fail("No se pudo copiar "+e.Name(), err)
		}
		fmt.Println("Instalado:", e.Name())
	}

	fmt.Println("\n¡Listo! Abre Geometry Dash desde Steam.")
	fmt.Println("Collab Free v" + version + " ya está en tus mods.")
	pause("\nPresiona Enter para cerrar...")
}
