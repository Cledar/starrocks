// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.starrocks.sql.optimizer.operator.scalar;

import com.google.common.collect.Lists;
import com.starrocks.catalog.Type;
import com.starrocks.sql.optimizer.base.ColumnRefSet;

import java.util.List;
import java.util.Objects;
import java.util.stream.Collectors;

import static com.starrocks.sql.optimizer.operator.OperatorType.MAP;

/**
 * MapOperator corresponds to MapExpr at the syntax level.
 * (k,v) -> map(k1,v1) a new map will created.
 */
public class MapOperator extends ArgsScalarOperator {
    public MapOperator(Type type, List<ScalarOperator> arguments) {
        super(MAP, type);
        this.arguments = arguments;
        incrDepth(arguments);
    }

    @Override
    public boolean isNullable() {
        return true;
    }

    @Override
    public String toString() {
        return arguments.stream().map(ScalarOperator::toString).collect(Collectors.joining(","));
    }

    @Override
    public ColumnRefSet getUsedColumns() {
        ColumnRefSet usedColumns = new ColumnRefSet();
        arguments.forEach(arg -> usedColumns.union(arg.getUsedColumns()));
        return usedColumns;
    }

    @Override
    public ScalarOperator clone() {
        MapOperator operator = (MapOperator) super.clone();
        // Deep copy here
        List<ScalarOperator> newArguments = Lists.newArrayList();
        this.arguments.forEach(p -> newArguments.add(p.clone()));
        operator.arguments = newArguments;
        return operator;
    }

    @Override
    public <R, C> R accept(ScalarOperatorVisitor<R, C> visitor, C context) {
        return visitor.visitMap(this, context);
    }

    @Override
    public int hashCodeSelf() {
        return Objects.hash(type);
    }
}
