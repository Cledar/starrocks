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

// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/load/loadv2/SparkRepository.java

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package com.starrocks.load.loadv2;

import com.google.common.base.Joiner;
import com.google.common.base.Preconditions;
import com.google.common.base.Strings;
import com.google.common.collect.Lists;
import com.starrocks.StarRocksFE;
import com.starrocks.analysis.BrokerDesc;
import com.starrocks.common.Config;
import com.starrocks.common.LoadException;
import com.starrocks.common.StarRocksException;
import com.starrocks.common.util.BrokerUtil;
import com.starrocks.fs.HdfsUtil;
import com.starrocks.thrift.TBrokerFileStatus;
import org.apache.commons.codec.digest.DigestUtils;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.List;
import java.util.Optional;

/*
 * SparkRepository represents the remote repository for spark archives uploaded by spark
 * The organization in repository is:
 *
 * * __spark_repository__/
 *   * __archive_1_0_0/
 *     * __lib_990325d2c0d1d5e45bf675e54e44fb16_spark-dpp.jar
 *     * __lib_7670c29daf535efe3c9b923f778f61fc_spark-2x.zip
 *   * __archive_2_2_0/
 *     * __lib_64d5696f99c379af2bee28c1c84271d5_spark-dpp.jar
 *     * __lib_1bbb74bb6b264a270bc7fca3e964160f_spark-2x.zip
 *   * __archive_3_2_0/
 *     * ...
 */
public class SparkRepository {
    private static final Logger LOG = LogManager.getLogger(SparkRepository.class);

    public static final String REPOSITORY_DIR = "__spark_repository__";
    public static final String PREFIX_ARCHIVE = "__archive_";
    public static final String PREFIX_LIB = "__lib_";
    public static final String SPARK_DPP_JAR = "spark-dpp-" + Config.spark_dpp_version + "-jar-with-dependencies.jar";
    public static final String SPARK_DPP = "spark-dpp-" + Config.spark_dpp_version + "-jar-with-dependencies";
    public static final String SPARK_2X = "spark-2x";

    private static final String PATH_DELIMITER = "/";
    private static final String FILE_NAME_SEPARATOR = "_";

    private static final String DPP_RESOURCE_DIR = "/spark-dpp/";
    private static final String SPARK_RESOURCE = "/jars/spark-2x.zip";

    private String remoteRepositoryPath;
    private BrokerDesc brokerDesc;
    private String localDppPath;
    private String localSpark2xPath;

    // Version of the spark dpp program in this cluster
    private String currentDppVersion;
    // Archive that current dpp version pointed to
    private SparkArchive currentArchive;

    public SparkRepository(String remoteRepositoryPath, BrokerDesc brokerDesc) {
        this.remoteRepositoryPath = remoteRepositoryPath;
        this.brokerDesc = brokerDesc;
        this.currentDppVersion = Config.spark_dpp_version;
        this.currentArchive = new SparkArchive(getRemoteArchivePath(currentDppVersion), currentDppVersion);
        this.localDppPath = StarRocksFE.STARROCKS_HOME_DIR + DPP_RESOURCE_DIR + SPARK_DPP_JAR;
        if (!Strings.isNullOrEmpty(Config.spark_resource_path)) {
            this.localSpark2xPath = Config.spark_resource_path;
        } else {
            this.localSpark2xPath = Config.spark_home_default_dir + SPARK_RESOURCE;
        }
    }

    public void prepare() throws LoadException {
        initRepository();
    }

    private void initRepository() throws LoadException {
        LOG.info("start to init remote repository. local dpp: {}", this.localDppPath);
        boolean needUpload = false;
        boolean needReplace = false;
        CHECK:
        {
            if (Strings.isNullOrEmpty(remoteRepositoryPath) || brokerDesc == null) {
                break CHECK;
            }

            if (!checkCurrentArchiveExists()) {
                needUpload = true;
                break CHECK;
            }

            // init current archive
            String remoteArchivePath = getRemoteArchivePath(currentDppVersion);
            List<SparkLibrary> libraries = Lists.newArrayList();
            getLibraries(remoteArchivePath, libraries);
            if (libraries.size() != 2) {
                needUpload = true;
                needReplace = true;
                break CHECK;
            }
            currentArchive.libraries.addAll(libraries);
            for (SparkLibrary library : currentArchive.libraries) {
                String localMd5sum = null;
                switch (library.libType) {
                    case DPP:
                        localMd5sum = getMd5String(localDppPath);
                        break;
                    case SPARK2X:
                        localMd5sum = getMd5String(localSpark2xPath);
                        break;
                    default:
                        Preconditions.checkState(false, "wrong library type: " + library.libType);
                        break;
                }
                if (!localMd5sum.equals(library.md5sum)) {
                    needUpload = true;
                    needReplace = true;
                    break;
                }
            }
        }

        if (needUpload) {
            uploadArchive(needReplace);
        }
        LOG.info("init spark repository success, current dppVersion={}, archive path={}, libraries size={}",
                currentDppVersion, currentArchive.remotePath, currentArchive.libraries.size());
    }

    private boolean checkCurrentArchiveExists() {
        boolean result = false;
        Preconditions.checkNotNull(remoteRepositoryPath);
        String remotePath = getRemoteArchivePath(currentDppVersion);
        try {
            if (brokerDesc.hasBroker()) {
                result = BrokerUtil.checkPathExist(remotePath, brokerDesc);
            } else {
                result = HdfsUtil.checkPathExist(remotePath, brokerDesc);
            }
            LOG.info("check archive exists in repository, {}", result);
        } catch (StarRocksException e) {
            LOG.warn("Failed to check remote archive exist, path={}, version={}", remotePath, currentDppVersion);
        }
        return result;
    }

    private void uploadArchive(boolean isReplace) throws LoadException {
        try {
            String remoteArchivePath = getRemoteArchivePath(currentDppVersion);
            if (isReplace) {
                if (brokerDesc.hasBroker()) {
                    BrokerUtil.deletePath(remoteArchivePath, brokerDesc);
                } else {
                    HdfsUtil.deletePath(remoteArchivePath, brokerDesc);
                }
                currentArchive.libraries.clear();
            }
            String srcFilePath = null;
            // upload dpp
            {
                // 1. upload dpp
                srcFilePath = localDppPath;
                String fileName = getFileName(PATH_DELIMITER, srcFilePath);
                String origFilePath = remoteArchivePath + PATH_DELIMITER +
                        assemblyFileName(PREFIX_LIB, "", fileName, "");
                upload(srcFilePath, origFilePath);
                // 2. rename dpp
                String md5sum = getMd5String(srcFilePath);
                long size = getFileSize(srcFilePath);
                String destFilePath = remoteArchivePath + PATH_DELIMITER +
                        assemblyFileName(PREFIX_LIB, md5sum, fileName, "");
                rename(origFilePath, destFilePath);
                currentArchive.libraries.add(new SparkLibrary(destFilePath, md5sum, SparkLibrary.LibType.DPP, size));
            }
            // upload spark2x
            {
                // 1. upload spark2x
                srcFilePath = localSpark2xPath;
                String fileName = getFileName(PATH_DELIMITER, srcFilePath);
                String origFilePath = remoteArchivePath + PATH_DELIMITER +
                        assemblyFileName(PREFIX_LIB, "", fileName, "");
                upload(srcFilePath, origFilePath);
                // 2. rename spark2x
                String md5sum = getMd5String(srcFilePath);
                long size = getFileSize(srcFilePath);
                String destFilePath = remoteArchivePath + PATH_DELIMITER +
                        assemblyFileName(PREFIX_LIB, md5sum, fileName, "");
                rename(origFilePath, destFilePath);
                currentArchive.libraries
                        .add(new SparkLibrary(destFilePath, md5sum, SparkLibrary.LibType.SPARK2X, size));
            }
            LOG.info("finished to upload archive to repository, currentDppVersion={}, path={}",
                    currentDppVersion, remoteArchivePath);
        } catch (StarRocksException e) {
            throw new LoadException(e.getMessage());
        }
    }

    private void getLibraries(String remoteArchivePath, List<SparkLibrary> libraries) throws LoadException {
        List<TBrokerFileStatus> fileStatuses = Lists.newArrayList();
        try {
            if (brokerDesc.hasBroker()) {
                BrokerUtil.parseFile(remoteArchivePath + "/*", brokerDesc, fileStatuses);
            } else {
                HdfsUtil.parseFile(remoteArchivePath + "/*", brokerDesc, fileStatuses);
            }
        } catch (StarRocksException e) {
            throw new LoadException(e.getMessage());
        }

        for (TBrokerFileStatus fileStatus : fileStatuses) {
            String fileName = getFileName(PATH_DELIMITER, fileStatus.path);
            if (!fileName.startsWith(PREFIX_LIB)) {
                continue;
            }

            // fileName should like:
            //      __lib_md5sum_spark-dpp-1.0.0-jar-with-dependencies.jar
            //      __lib_md5sum_spark-2x.zip
            String[] libArg = unwrap(PREFIX_LIB, fileName).split(FILE_NAME_SEPARATOR);
            if (libArg.length != 2) {
                continue;
            }
            String md5sum = libArg[0];
            if (Strings.isNullOrEmpty(md5sum)) {
                continue;
            }
            String type = libArg[1];
            SparkLibrary.LibType libType = null;
            if (type.equals(SPARK_DPP)) {
                libType = SparkLibrary.LibType.DPP;
            } else if (type.equals(SPARK_2X)) {
                libType = SparkLibrary.LibType.SPARK2X;
            } else {
                throw new LoadException("Invalid library type: " + type);
            }
            SparkLibrary remoteFile = new SparkLibrary(fileStatus.path, md5sum, libType, fileStatus.size);
            libraries.add(remoteFile);
            LOG.info("get Libraries from remote archive, archive path={}, library={}, md5sum={}, size={}",
                    remoteArchivePath, remoteFile.remotePath, remoteFile.md5sum, remoteFile.size);
        }
    }

    public String getMd5String(String filePath) throws LoadException {
        File file = new File(filePath);
        String md5sum = null;
        try {
            md5sum = DigestUtils.md5Hex(new FileInputStream(file));
            Preconditions.checkNotNull(md5sum);
            LOG.debug("get md5sum from file {}, md5sum={}", filePath, md5sum);
            return md5sum;
        } catch (FileNotFoundException e) {
            throw new LoadException("file " + filePath + " does not exist");
        } catch (IOException e) {
            throw new LoadException("failed to get md5sum from file " + filePath);
        }
    }

    public long getFileSize(String filePath) throws LoadException {
        File file = new File(filePath);
        long size = file.length();
        if (size <= 0) {
            throw new LoadException("failed to get size from file " + filePath);
        }
        return size;
    }

    private void upload(String srcFilePath, String destFilePath) throws LoadException {
        try {
            if (brokerDesc.hasBroker()) {
                BrokerUtil.writeFile(srcFilePath, destFilePath, brokerDesc);
            } else {
                HdfsUtil.writeFile(srcFilePath, destFilePath, brokerDesc);
            }
            LOG.info("finished to upload file, localPath={}, remotePath={}", srcFilePath, destFilePath);
        } catch (StarRocksException e) {
            throw new LoadException("failed to upload lib to repository, srcPath=" + srcFilePath +
                    " destPath=" + destFilePath + " message=" + e.getMessage());
        }
    }

    private void rename(String origFilePath, String destFilePath) throws LoadException {
        try {
            if (brokerDesc.hasBroker()) {
                BrokerUtil.rename(origFilePath, destFilePath, brokerDesc);
            } else {
                HdfsUtil.rename(origFilePath, destFilePath, brokerDesc);
            }
            LOG.info("finished to rename file, originPath={}, destPath={}", origFilePath, destFilePath);
        } catch (StarRocksException e) {
            throw new LoadException("failed to rename file from " + origFilePath + " to " + destFilePath +
                    ", message=" + e.getMessage());
        }
    }

    public SparkArchive getCurrentArchive() {
        return currentArchive;
    }

    private static String getFileName(String delimiter, String path) {
        return path.substring(path.lastIndexOf(delimiter) + 1);
    }

    // input:   __lib_md5sum_spark-dpp-1.0.0-jar-with-dependencies.jar
    // output:  md5sum_spark-dpp-1.0.0-jar-with-dependencies
    private static String unwrap(String prefix, String fileName) {
        int pos = fileName.lastIndexOf(".");
        return fileName.substring(prefix.length(), pos);
    }

    private static String joinPrefix(String prefix, String fileName) {
        return prefix + fileName;
    }

    // eg:
    // __lib_979bfbcb9469888f1c5539e168d1925d_spark-2x.zip
    public static String assemblyFileName(String prefix, String md5sum, String fileName, String suffix) {
        return prefix + md5sum + FILE_NAME_SEPARATOR + fileName + suffix;
    }

    // eg:
    // .../__spark_repository__/__archive_1_0_0
    private String getRemoteArchivePath(String version) {
        return Joiner.on(PATH_DELIMITER).join(remoteRepositoryPath, joinPrefix(PREFIX_ARCHIVE, version));
    }

    // Represents a remote directory contains the uploaded libraries
    // an archive is named as __archive_{dppVersion}.
    // e.g. __archive_1_0_0/
    //        \ __lib_990325d2c0d1d5e45bf675e54e44fb16_spark-dpp.jar
    // *      \ __lib_7670c29daf535efe3c9b923f778f61fc_spark-2x.zip
    public static class SparkArchive {
        public String remotePath;
        public String version;
        public List<SparkLibrary> libraries;

        public SparkArchive(String remotePath, String version) {
            this.remotePath = remotePath;
            this.version = version;
            this.libraries = Lists.newArrayList();
        }

        public SparkLibrary getDppLibrary() {
            SparkLibrary result = null;
            Optional<SparkLibrary> library = libraries.stream().
                    filter(lib -> lib.libType == SparkLibrary.LibType.DPP).findFirst();
            if (library.isPresent()) {
                result = library.get();
            }
            return result;
        }

        public SparkLibrary getSpark2xLibrary() {
            SparkLibrary result = null;
            Optional<SparkLibrary> library = libraries.stream().
                    filter(lib -> lib.libType == SparkLibrary.LibType.SPARK2X).findFirst();
            if (library.isPresent()) {
                result = library.get();
            }
            return result;
        }
    }

    // Represents a uploaded remote file that save in the archive
    // a library refers to the dependency of DPP program or spark platform
    // named as __lib_{md5sum}_spark_{type}.{jar/zip}.
    // e.g. __lib_990325d2c0d1d5e45bf675e54e44fb16_spark-dpp.jar
    //      __lib_7670c29daf535efe3c9b923f778f61fc_spark-2x.zip
    public static class SparkLibrary {
        public String remotePath;
        public String md5sum;
        public long size;
        public LibType libType;

        public enum LibType {
            DPP, SPARK2X
        }

        public SparkLibrary(String remotePath, String md5sum, LibType libType, long size) {
            this.remotePath = remotePath;
            this.md5sum = md5sum;
            this.libType = libType;
            this.size = size;
        }
    }
}
