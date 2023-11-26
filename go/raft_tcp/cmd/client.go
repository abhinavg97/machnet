package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"github.com/buger/jsonparser"
	"github.com/golang/glog"
	"math"
	"math/rand"
	"net"
	"os"
	"runtime/pprof"
	"strings"
	"time"
)

var (
	localHostname  = flag.String("local_hostname", "", "Local hostname in the hosts JSON file.")
	remoteHostname = flag.String("remote_hostname", "", "Local hostname in the hosts JSON file.")
	appPort        = flag.String("app_port", "888", "Port to listen on for application traffic.")
	configJson     = flag.String("config_json", "./servers.json", "Path to the JSON file containing the hosts config.")
	cpuProfile     = flag.String("cpuProfile", "", "write cpu profile to file")
	keySize        = flag.Int("key_size", 16, "Key size")
	valueSize      = flag.Int("value_size", 64, "Value size")
)

type Payload struct {
	Key   string
	Value string
}

func main() {
	flag.Parse()
	if *cpuProfile != "" {
		f, err := os.Create(*cpuProfile)
		if err != nil {
			glog.Fatalf("Main: failed to create cpu profile: %+v", err)
		}
		err = pprof.StartCPUProfile(f)
		if err != nil {
			glog.Errorf("Main: failed to start CPU profiler: %v", err)
		}
		defer pprof.StopCPUProfile()
	}

	jsonBytes, err := os.ReadFile(*configJson)
	if err != nil {
		glog.Fatalf("Main: failed to read config file: %v", err)
	}

	// Parse the json file to get the local ip and remote ip.
	remoteIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", *remoteHostname, "ipv4_addr")
	remoteAddr := remoteIp + ":" + *appPort

	conn, err := net.Dial("tcp", remoteAddr)
	if err != nil {
		glog.Fatalf("Main: failed to connect to %v: %v", remoteAddr, err)
	}
	defer func(conn net.Conn) {

		if err := conn.Close(); err != nil {
			glog.Errorf("Main: failed to close connection: %v", err)
		}
	}(conn)

	for {
		// send msg
		key := generateRandomKey(*keySize)
		value := generateRandomValue(*valueSize)
		payload := Payload{
			key, value,
		}
		if err := json.NewEncoder(conn).Encode(payload); err != nil {
			glog.Errorf("Main: failed to send payload: %v", err)
		}
		// recv success
		response, err := bufio.NewReader(conn).ReadString('\n')
		if err != nil {
			glog.Errorf("Main: failed to read response: %v", err)
		}
		response = strings.TrimSpace(response)
		glog.Infof("Main: received response: %v", response)
	}

}
func generateRandomKey(length int) string {
	source := rand.NewSource(time.Now().UnixNano())
	randRange := rand.New(source)
	max_ := int(math.Pow10(length)) - 1
	number := randRange.Intn(max_)
	return fmt.Sprintf("%0*d", length, number)
}

func generateRandomValue(length int) string {
	const charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
	source := rand.NewSource(time.Now().UnixNano())
	randRange := rand.New(source)
	b := make([]byte, length)
	for i := range b {
		b[i] = charset[randRange.Intn(len(charset))]
	}
	return string(b)
}
