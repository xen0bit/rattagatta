package main

import (
	"bufio"
	"flag"
	"fmt"
	"log"
	"os"

	"github.com/xen0bit/rattagatt/rg-loader/pkg/csvloader"
	"github.com/xen0bit/rattagatt/rg-loader/pkg/dbtools"
	"github.com/xen0bit/rattagatt/rg-loader/pkg/jlog"
)

func main() {
	inLog := flag.String("i", "", "Input file path to log.jsonl")
	outDb := flag.String("o", "", "Output file path for SQLite database")

	flag.Parse()

	if *inLog == "" || *outDb == "" {
		fmt.Println("read the help with -h")
		os.Exit(1)
	}

	sqliteconn := dbtools.NewSqliteConn(*outDb)
	defer sqliteconn.DB.Close()
	if err := sqliteconn.CreateTables(); err != nil {
		log.Fatal(err)
	}

	//OUI
	file, err := os.Open("metadata_generator/oui.csv")
	if err != nil {
		panic(err)
	}
	defer file.Close()

	ouich := make(chan dbtools.OuiRow)

	//Producer
	go func(ch chan dbtools.OuiRow) {
		o := csvloader.NewScanner(bufio.NewReader(file))
		for o.Scan() {
			ch <- dbtools.OuiRow{
				Oui:         o.Text("oui"),
				CompanyName: o.Text("company_name"),
				AddressOne:  o.Text("address1"),
				AddressTwo:  o.Text("address2"),
				Country:     o.Text("country"),
			}
		}
		close(ch)
	}(ouich)

	//Consumer
	if err := sqliteconn.InsertOuiData(ouich); err != nil {
		log.Fatal(err)
	}

	//GATT Characteristics
	file, err = os.Open("metadata_generator/gattchars.csv")
	if err != nil {
		panic(err)
	}
	defer file.Close()

	gattch := make(chan dbtools.GattRow)

	//Producer
	go func(ch chan dbtools.GattRow) {
		o := csvloader.NewScanner(bufio.NewReader(file))
		for o.Scan() {
			ch <- dbtools.GattRow{
				CharacteristicUUID: o.Text("characteristic_uuid"),
				CharacteristicName: o.Text("characteristic_name"),
			}
		}
		close(ch)
	}(gattch)

	//Consumer
	if err := sqliteconn.InsertGattData(gattch); err != nil {
		log.Fatal(err)
	}

	//Load the logs

	logch := make(chan dbtools.RecordRow)

	//Producer
	go jlog.ReadLog(*inLog, logch)

	//Consumer
	if err := sqliteconn.InsertLogData(logch); err != nil {
		log.Fatal(err)
	}

	//Generate records table
	if err := sqliteconn.GenerateRecords(); err != nil {
		log.Fatal(err)
	}
}
