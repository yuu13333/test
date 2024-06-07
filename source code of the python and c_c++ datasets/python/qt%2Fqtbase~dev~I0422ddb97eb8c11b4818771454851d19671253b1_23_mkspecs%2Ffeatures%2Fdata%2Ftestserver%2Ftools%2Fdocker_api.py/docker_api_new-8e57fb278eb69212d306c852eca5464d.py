#!/usr/bin/env python

#############################################################################
##
## Copyright (C) 2018 The Qt Company Ltd.
## Contact: https://www.qt.io/licensing/
##
## This file is part of the test suite of the Qt Toolkit.
##
## $QT_BEGIN_LICENSE:LGPL$
## Commercial License Usage
## Licensees holding valid commercial Qt licenses may use this file in
## accordance with the commercial license agreement provided with the
## Software or, alternatively, in accordance with the terms contained in
## a written agreement between you and The Qt Company. For licensing terms
## and conditions see https://www.qt.io/terms-conditions. For further
## information use the contact form at https://www.qt.io/contact-us.
##
## GNU Lesser General Public License Usage
## Alternatively, this file may be used under the terms of the GNU Lesser
## General Public License version 3 as published by the Free Software
## Foundation and appearing in the file LICENSE.LGPL3 included in the
## packaging of this file. Please review the following information to
## ensure the GNU Lesser General Public License version 3 requirements
## will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
##
## GNU General Public License Usage
## Alternatively, this file may be used under the terms of the GNU
## General Public License version 2.0 or (at your option) the GNU General
## Public license version 3 or any later version approved by the KDE Free
## Qt Foundation. The licenses are as published by the Free Software
## Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
## included in the packaging of this file. Please review the following
## information to ensure the GNU General Public License requirements will
## be met: https://www.gnu.org/licenses/gpl-2.0.html and
## https://www.gnu.org/licenses/gpl-3.0.html.
##
## $QT_END_LICENSE$
##
#############################################################################

import logging
import socket
import os
from subprocess import Popen, PIPE, check_call, check_output
from tools import SERVER_NAME, DOMAIN_NAME, JsonConfig
from tools.utility import str_encode_hex

class DockerConfig(JsonConfig):
    """ Docker config items """

    def __init__(self, server):
        JsonConfig.__init__(self, server)

    @property
    def docker_file(self):
        """ build server configurations """
        config_list = []

        # NOTE: FROM must be the first command of DockerFile
        build_version = self.version
        if build_version:
            config_list.append("FROM ubuntu:%s" % build_version)

        maintainer = self.maintainer
        if maintainer:
            config_list.append("MAINTAINER %s" % maintainer)

        if self.update:
            config_list.append('RUN apt-get update')

        install_list = self.install
        if install_list:
            config_list.append("RUN apt-get install %s -y" % install_list)

        config_list.extend(self.copy_commands)
        config_list.extend(self.get_config('docker_build_command'))
        return config_list

    @property
    def nwk_links(self):
        """ add link to another container (--link) """
        links = ''
        for link_server in self.link:
            links += "--link %s " % self.get_server_name(link_server)

        link_backend = self.backend
        if link_backend:
            dummy_id = self.get_server_name(link_backend)
            dummy_alias = socket.getfqdn()
            return '{links} --link {dummy_id}:{dummy_alias}'.format(**locals())
        else:
            return links

    @property
    def export_ports(self):
        """ publish a container's port to the host (-p) """
        port_str = ''
        if os.environ.has_key("TESTSERVER_BIND_LOCAL"):
            for dummy_port in self.port:
                port_str += '-p {dummy_port}:{dummy_port} '.format(**locals())
        else:
            for dummy_port in self.port:
                port_str += '-p {dummy_port} '.format(**locals())
        return port_str

    @property
    def copy_commands(self):
        """ copy files from the host to a container """
        copy_cmd = []
        for copy_file in self.copy:
            dummy_str = str_encode_hex(open(copy_file.src, 'r').read())
            copy_cmd.append(('RUN echo "\'{dummy_str}\'" | xargs echo -e > {copy_file.dst};'
                             'chmod {copy_file.mode} {copy_file.dst}').format(**locals()))
        return copy_cmd

    @property
    def image_name(self):
        """ get image name """
        return self.get_image_name(self.module)

    @property
    def server_name(self):
        """ get server name """
        return self.get_server_name(self.module)

    @property
    def host_name(self):
        """ get server hostname """
        return self.get_host_name(self.module)

    @staticmethod
    def get_image_name(server):
        """ static get image name """
        return '{0}:{1}'.format(SERVER_NAME, server)

    @staticmethod
    def get_server_name(server):
        """ static get server name """
        return '{0}-{1}'.format(SERVER_NAME, server)

    @staticmethod
    def get_host_name(server):
        """ static get server hostname """
        return '{0}.{1}'.format(server, DOMAIN_NAME)

def docker_build(server_list):
    """ build up server images """
    base_file = DockerConfig('.').docker_file

    for server in server_list:
        config = DockerConfig(server)
        logging.info('[docker_build] (%s)', config.server_name)

        docker_pipe = Popen(("docker build - --quiet -t %s" % config.image_name).split(),
                            stdin=PIPE, stdout=PIPE, stderr=PIPE)

        docker_file = '\n'.join(base_file + config.docker_file)
        _, stderr = docker_pipe.communicate(input=docker_file)

        if stderr:
            print stderr
            exit(-1) # terminate if docker build fail

def docker_run(server_list):
    """ bring up all the server containers for testing """
    docker_rm(server_list) # remove existed container for a clean test

    for server in server_list:
        config = DockerConfig(server)
        logging.info('[docker_run] (%s) %s', config.server_name, config.host_name)

        docker_pipe = Popen(('docker run -d -h {config.host_name} {config.export_ports} '
                             '{config.nwk_links} --name {config.server_name} '
                             '{config.image_name}'.format(**locals())).split(),
                            stdout=PIPE, stderr=PIPE)

        _, stderr = docker_pipe.communicate()

        if stderr:
            print stderr
            exit(-1) # terminate if docker build fail

def docker_exec(server_list):
    """ make sure the server containers are ready for testing """
    script = '/post_startup.py'
    retry = 60

    for server in server_list:
        config = DockerConfig(server)
        logging.info('[docker_exec] (%s) %s, %d', config.server_name, script, retry)

        # Note:
        # Do not use stdout=PIPE or stderr=PIPE with this function as that can
        # deadlock based on the child process output volume. Use Popen with the
        # communicate() method when you need pipes.
        check_call(('docker exec {config.server_name} {script} '
                    '{config.ensure} -c {retry}'.format(**locals())).split())

def docker_rm(server_list):
    """ delete all the server containers for a clean test """
    for server in server_list:
        logging.info('[docker_rm] (%s)', DockerConfig.get_server_name(server))

        docker_pipe = Popen(("docker rm -f %s" % DockerConfig.get_server_name(server)).split(),
                            stdout=PIPE, stderr=PIPE)

        docker_pipe.communicate() # ignore docker remove container error

def docker_rmi(server_list):
    """ delete all the server image for a clean build """
    docker_rm(server_list) # stop depending containers then remove images

    for server in server_list:
        logging.info('[docker_rmi] (%s)', DockerConfig.get_image_name(server))

        docker_pipe = Popen(("docker rmi %s" % DockerConfig.get_image_name(server)).split(),
                            stdout=PIPE, stderr=PIPE)

        docker_pipe.communicate() # ignore docker remove image error

def docker_version():
    """ retrieve docker version """
    try:
        return check_output(['docker', '-v'])
    except OSError:
        logging.error('[docker_version] not executable')
        return ''
