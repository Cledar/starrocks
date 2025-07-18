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

package com.starrocks.lake.compaction;

import com.starrocks.proto.AbortCompactionRequest;
import com.starrocks.proto.CompactRequest;
import com.starrocks.proto.CompactResponse;
import com.starrocks.rpc.LakeService;
import mockit.Expectations;
import mockit.Mocked;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;

import java.lang.reflect.Field;
import java.util.Arrays;
import java.util.concurrent.Future;

public class CompactionTaskTest {


    @Test
    public void testAbortThrowException(@Mocked LakeService lakeService) {
        CompactRequest request = new CompactRequest();
        request.tabletIds = Arrays.asList(1L);
        request.txnId = 1000L;
        request.version = 10L;
        CompactionTask task = new CompactionTask(10043, lakeService, request);
        new Expectations() {
            {
                lakeService.abortCompaction((AbortCompactionRequest) any);
                result = new RuntimeException("channel inactive error");
            }
        };

        task.abort();
    }

    @Test
    public void testAbort(@Mocked LakeService lakeService) {
        CompactRequest request = new CompactRequest();
        request.tabletIds = Arrays.asList(1L);
        request.txnId = 1000L;
        request.version = 10L;
        CompactionTask task = new CompactionTask(10043, lakeService, request);
        new Expectations() {
            {
                lakeService.abortCompaction((AbortCompactionRequest) any);
                result = null; // unused
            }
        };

        task.abort();
    }

    @Test
    public void testGetSuccessCompactInputFileSizeSuccess(
            @Mocked Future<CompactResponse> mockFuture,
            @Mocked LakeService lakeService) throws Exception {
        CompactRequest request = new CompactRequest();
        request.tabletIds = Arrays.asList(1L);
        request.txnId = 1000L;
        request.version = 10L;
        CompactionTask task = new CompactionTask(10043, lakeService, request);

        CompactResponse mockResponse = new CompactResponse();
        mockResponse.successCompactionInputFileSize = 100L;
        Field field = task.getClass().getDeclaredField("responseFuture");
        field.setAccessible(true);
        field.set(task, mockFuture);
        
        new Expectations() {
            {
                mockFuture.get(); 
                result = mockResponse;
                mockFuture.isDone();
                result = true;
            }
        };

        Assertions.assertEquals(100L, task.getSuccessCompactInputFileSize());
    }
}
