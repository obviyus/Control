/*
 * === This file is part of ALICE O² ===
 *
 * Copyright 2018 CERN and copyright holders of ALICE O².
 * Author: Teo Mrnjavac <teo.mrnjavac@cern.ch>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In applying this license CERN does not waive the privileges and
 * immunities granted to it by virtue of its status as an
 * Intergovernmental Organization or submit itself to any jurisdiction.
 */

package workflow

import "github.com/AliceO2Group/Control/core/task"

type aggregatorRole struct {
	roleBase
	aggregator
}

func (r *aggregatorRole) UnmarshalYAML(unmarshal func(interface{}) error) (err error) {
	role := aggregatorRole{}
	err = unmarshal(&role) // internally calls controllableRoles.UnmarshalYAML
	if err != nil {
		return
	}
	*r = role
	for _, v := range r.Roles {
		v.setParent(r)
	}
	return
}

func (r *aggregatorRole) copy() copyable {
	rCopy := aggregatorRole{
		roleBase: *r.roleBase.copy().(*roleBase),
		aggregator: *r.aggregator.copy().(*aggregator),
	}
	return &rCopy
}

func (r *aggregatorRole) setParent(role updatableRole) {
	r.parent = role
}

func (r *aggregatorRole) updateStatus(s task.Status) {
	if r == nil {
		return
	}
	r.status.merge(s, r)
	if r.parent != nil {
		r.parent.updateStatus(r.status.get())
	}
}

func (r *aggregatorRole) updateState(s task.State) {
	if r == nil {
		return
	}
	r.state.merge(s, r)
	if r.parent != nil {
		r.parent.updateState(r.state.get())
	}
}