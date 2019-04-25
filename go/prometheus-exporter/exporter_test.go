package main

import (
	"testing"
)

func TestDeletingLabels(t *testing.T) {
	s := stream{
		App:                 "a",
		Env:                 "b",
		IgnoredRequestURI:   "/_",
		BackendOnlyRequests: "",
		APIRequests:         []string{},
	}
	c := newCollector(s.AppEnv(), s)
	metrics1 := &metric{
		kind: Log,
		props: map[string]string{
			"application": "a",
			"environment": "b",
			"metric":      "http",
			"code":        "200",
			"http_method": "GET",
			"cluster":     "c",
			"datacenter":  "d",
			"action":      "murks",
		},
		value: 5.7,
	}
	metrics2 := &metric{
		kind: Log,
		props: map[string]string{
			"application": "a",
			"environment": "b",
			"metric":      "http",
			"code":        "200",
			"http_method": "GET",
			"cluster":     "d",
			"datacenter":  "e",
			"action":      "marks",
		},
		value: 7.7,
	}
	metrics3 := &metric{
		kind: Log,
		props: map[string]string{
			"application": "a",
			"environment": "b",
			"metric":      "job",
			"code":        "200",
			"cluster":     "d",
			"datacenter":  "e",
			"action":      "marks",
		},
		value: 3.1,
	}
	metrics4 := &metric{
		kind: Log,
		props: map[string]string{
			"application": "a",
			"environment": "b",
			"metric":      "job",
			"code":        "200",
			"cluster":     "d",
			"datacenter":  "e",
			"action":      "marks"},
		value: 4.4,
	}
	c.recordLogMetrics(metrics1)
	c.recordLogMetrics(metrics2)
	c.recordLogMetrics(metrics3)
	c.recordLogMetrics(metrics4)
	if !c.removeAction("marks") {
		t.Errorf("could not remove action: %s", "marks")
	}
	if c.removeAction("schnippi") {
		t.Errorf("could remove non existing action : %s", "schnippi")
	}
}
