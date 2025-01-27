//
// (C) Copyright 2019 Intel Corporation.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
// The Government's rights to use, modify, reproduce, release, perform, display,
// or disclose this software are subject to the terms of the Apache License as
// provided in Contract No. 8F-30005.
// Any reproduction of computer software, computer software documentation, or
// portions thereof marked with this legend must also reproduce the markings.
//

package server

import (
	"testing"

	"github.com/daos-stack/daos/src/control/logging"
	"github.com/daos-stack/daos/src/control/server/ioserver"
	"github.com/daos-stack/daos/src/control/server/storage"
	"github.com/daos-stack/daos/src/control/server/storage/scm"
)

func defaultMockControlService(t *testing.T, log logging.Logger) *ControlService {
	c := defaultMockConfig(t)
	return mockControlService(t, log, c, nil, nil)
}

// mockControlService takes cfgs for tuneable scm and sys provider behaviour but
// default nvmeStorage behaviour (cs.nvoe can be subsequently replaced in test).
func mockControlService(t *testing.T, log logging.Logger, cfg *Configuration, mbc *scm.MockBackendConfig, msc *scm.MockSysConfig) *ControlService {
	t.Helper()

	cs := ControlService{
		StorageControlService: *NewStorageControlService(log, cfg.ext,
			defaultMockNvmeStorage(log, cfg.ext),
			scm.NewMockProvider(log, mbc, msc),
			cfg.Servers,
		),
		harness: &IOServerHarness{
			log: log,
		},
	}

	scmProvider := cs.StorageControlService.scm
	for _, srvCfg := range cfg.Servers {
		bp, err := storage.NewBdevProvider(log, "", &srvCfg.Storage.Bdev)
		if err != nil {
			t.Fatal(err)
		}
		runner := ioserver.NewRunner(log, srvCfg)
		instance := NewIOServerInstance(log, bp, scmProvider, nil, runner)
		if err := cs.harness.AddInstance(instance); err != nil {
			t.Fatal(err)
		}
	}

	return &cs
}
