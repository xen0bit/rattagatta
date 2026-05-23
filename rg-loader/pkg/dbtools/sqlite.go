package dbtools

import (
	"database/sql"
	"log"
	"os"

	_ "github.com/mattn/go-sqlite3"
)

type SqliteConn struct {
	err error
	DB  *sql.DB
}

type OuiRow struct {
	Oui         string
	CompanyName string
	AddressOne  string
	AddressTwo  string
	Country     string
}

type GattRow struct {
	CharacteristicUUID string
	CharacteristicName string
}

type AdvRow struct {
	Mac              string
	Name             string
	ManufacturerData []byte
}

type SvcRow struct {
	Mac         string
	ServiceUUID string
}

type CharRow struct {
	Mac                string
	ServiceUUID        string
	CharacteristicUUID string
	ReadValue          []byte
}

type RecordRow struct {
	Gid                int
	Mac                string
	AddressType        int
	Name               string
	Rssi               int
	ManufacturerData   []byte
	Connectable        int
	ServiceUUID        string
	CharacteristicUUID string
	Properties         int
	ReadValue          []byte
}

func NewSqliteConn(dbPath string) *SqliteConn {
	os.Remove(dbPath)
	db, err := sql.Open("sqlite3", "file:"+dbPath+"?cache=shared")
	if err != nil {
		log.Fatal(err)
	}
	_, err = db.Exec("PRAGMA journal_mode = OFF;")
	if err != nil {
		log.Fatal(err)
	}
	_, err = db.Exec("PRAGMA synchronous = 0")
	if err != nil {
		log.Fatal(err)
	}
	_, err = db.Exec("PRAGMA cache_size = 1000000")
	if err != nil {
		log.Fatal(err)
	}
	_, err = db.Exec("PRAGMA locking_mode = EXCLUSIVE")
	if err != nil {
		log.Fatal(err)
	}
	_, err = db.Exec("PRAGMA temp_store = MEMORY;")
	if err != nil {
		log.Fatal(err)
	}
	//defer db.Close()
	return &SqliteConn{err, db}
}

func (sc *SqliteConn) CreateTables() error {
	stmts := []string{
		`CREATE TABLE "logs" (
			id        INTEGER NOT NULL PRIMARY KEY,
			"gid"     INTEGER,
			"mac"     TEXT,
			"addr_type" INTEGER,
			"name"    TEXT,
			"rssi"    INTEGER,
			"man"     BLOB,
			"conn"    INTEGER,
			"svc"     TEXT,
			"chr"     TEXT,
			"props"   INTEGER,
			"val"     BLOB
		);`,
		`CREATE TABLE "oui_lookup" (
			oui          TEXT NOT NULL PRIMARY KEY,
			company_name TEXT,
			address1     TEXT,
			address2     TEXT,
			country      TEXT
		);`,
		`CREATE TABLE "char_lookup" (
			characteristic_uuid TEXT NOT NULL PRIMARY KEY,
			characteristic_name TEXT
		);`,
	}
	for _, s := range stmts {
		if _, err := sc.DB.Exec(s); err != nil {
			log.Printf("CreateTables: %q\n%s\n", err, s)
			return err
		}
	}
	return nil
}

func (sc *SqliteConn) InsertOuiData(chnl chan OuiRow) error {
	tx, err := sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err := tx.Prepare("INSERT INTO oui_lookup (oui, company_name, address1, address2, country) VALUES (?,?,?,?,?)")
	if err != nil {
		return err
	}
	defer stmt.Close()
	for v := range chnl {
		_, err = stmt.Exec(v.Oui, v.CompanyName, v.AddressOne, v.AddressTwo, v.Country)
		if err != nil {
			return err
		}
	}
	tx.Commit()
	return nil
}

func (sc *SqliteConn) InsertGattData(chnl chan GattRow) error {
	tx, err := sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err := tx.Prepare("INSERT INTO char_lookup (characteristic_uuid,characteristic_name) VALUES (?,?)")
	if err != nil {
		return err
	}
	defer stmt.Close()
	for v := range chnl {
		_, err = stmt.Exec(v.CharacteristicUUID, v.CharacteristicName)
		if err != nil {
			return err
		}
	}
	tx.Commit()
	return nil
}

func (sc *SqliteConn) InsertAdvData(chnl chan AdvRow) error {
	tx, err := sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err := tx.Prepare("INSERT INTO advs (mac, name, manufacturer_data) VALUES (?,?,?)")
	if err != nil {
		return err
	}
	defer stmt.Close()
	for v := range chnl {
		_, err = stmt.Exec(v.Mac, v.Name, v.ManufacturerData)
		if err != nil {
			return err
		}
	}
	tx.Commit()
	return nil
}

func (sc *SqliteConn) InsertSvcData(chnl chan SvcRow) error {
	tx, err := sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err := tx.Prepare("INSERT INTO svcs (mac, service_uuid) VALUES (?,?)")
	if err != nil {
		return err
	}
	defer stmt.Close()
	for v := range chnl {
		_, err = stmt.Exec(v.Mac, v.ServiceUUID)
		if err != nil {
			return err
		}
	}
	tx.Commit()
	return nil
}

func (sc *SqliteConn) InsertCharData(chnl chan CharRow) error {
	tx, err := sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err := tx.Prepare("INSERT INTO chars (mac, service_uuid, characteristic_uuid, read_val) VALUES (?,?,?,?)")
	if err != nil {
		return err
	}
	defer stmt.Close()
	for v := range chnl {
		_, err = stmt.Exec(v.Mac, v.ServiceUUID, v.CharacteristicUUID, v.ReadValue)
		if err != nil {
			return err
		}
	}
	tx.Commit()
	return nil
}

func (sc *SqliteConn) InsertLogData(chnl chan RecordRow) error {
	tx, err := sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err := tx.Prepare("INSERT INTO logs (gid, mac, addr_type, name, rssi, man, conn, svc, chr, props, val) VALUES (?,?,?,?,?,?,?,?,?,?,?)")
	if err != nil {
		return err
	}
	defer stmt.Close()
	for v := range chnl {
		_, err = stmt.Exec(v.Gid, v.Mac, v.AddressType, v.Name, v.Rssi, v.ManufacturerData, v.Connectable, v.ServiceUUID, v.CharacteristicUUID, v.Properties, v.ReadValue)
		if err != nil {
			return err
		}
	}
	tx.Commit()
	return nil
}

func (sc *SqliteConn) GenerateRecords() error {
	logDedupe := `
	CREATE TABLE IF NOT EXISTS logs_dedupe AS
	select *
	from logs
	group by gid, mac, addr_type, name, man, conn, svc, chr, props, val;
	`

	records := `
	CREATE TABLE IF NOT EXISTS records AS
	SELECT
		ld.gid,
		ld.mac,
		ld.addr_type,
		ld.name,
		ld.man  AS manufacturer_data,
		ld.conn AS connectable,
		ld.svc  AS service_uuid,
		ld.chr  AS characteristic_uuid,
		cl.characteristic_name,
		ld.props AS properties,
		ld.val
	FROM logs_dedupe ld
	LEFT JOIN char_lookup cl ON ld.chr = cl.characteristic_uuid
	GROUP BY
		ld.gid, ld.mac, ld.name, ld.man, ld.conn,
		ld.svc, ld.chr, ld.props, ld.val
	ORDER BY
		ld.gid, ld.mac, ld.conn, ld.svc, ld.chr, ld.props, ld.val;
	`

	dropLogDedupe := `DROP TABLE logs_dedupe;`

	// vac := `VACUUM;`

	//Logs dedupe
	tx, err := sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err := tx.Prepare(logDedupe)
	if err != nil {
		return err
	}
	defer stmt.Close()
	_, err = stmt.Exec()
	if err != nil {
		return err
	}
	tx.Commit()

	//Records
	tx, err = sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err = tx.Prepare(records)
	if err != nil {
		return err
	}
	defer stmt.Close()
	_, err = stmt.Exec()
	if err != nil {
		return err
	}
	tx.Commit()

	//drop
	tx, err = sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err = tx.Prepare(dropLogDedupe)
	if err != nil {
		return err
	}
	defer stmt.Close()
	_, err = stmt.Exec()
	if err != nil {
		return err
	}
	tx.Commit()

	//vac
	// tx, err = sc.DB.Begin()
	// if err != nil {
	// 	return err
	// }
	// stmt, err = tx.Prepare(vac)
	// if err != nil {
	// 	return err
	// }
	// defer stmt.Close()
	// _, err = stmt.Exec()
	// if err != nil {
	// 	return err
	// }
	// tx.Commit()

	return nil
}
