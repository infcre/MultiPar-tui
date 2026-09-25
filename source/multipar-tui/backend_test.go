package main

import (
	"reflect"
	"testing"
)

// par2j treats an input that ends in a separator as "record this empty folder
// and do not look inside it", so a set built from "/data/docs/" holds nothing
// but a 0-byte folder entry while still reporting success.  The front end owns
// the fix because par2j itself must keep the upstream reading.
func TestArgsStripsTrailingSlashFromInputs(t *testing.T) {
	j := Job{
		Op:         OpCreate,
		ParFile:    "/data/backup/docs.par2",
		Inputs:     []string{"/data/docs/", "/data/docs//", "/data/photos", "/"},
		BlockSize:  65536,
		Redundancy: 20,
	}

	got := j.Args()
	want := []string{
		"c", "-ss65536", "-rr20", "/data/backup/docs.par2",
		"/data/docs", "/data/docs", "/data/photos", "/",
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("Args() = %q\n           want %q", got, want)
	}
}

// The option parser stops reading options as soon as the marker style changes,
// so every option the front end emits has to use the same '-' prefix.
func TestArgsKeepsOneOptionPrefix(t *testing.T) {
	j := Job{
		Op:      OpVerify,
		ParFile: "/data/backup/docs.par2",
		BaseDir: "/data/docs",
	}

	args := j.Args()
	for i, a := range args {
		if a == j.ParFile {
			break // everything from here on is a file name, not an option
		}
		if i > 0 && len(a) > 1 && a[0] == '/' {
			t.Fatalf("Args() emitted a slash-prefixed option %q", a)
		}
	}
}
