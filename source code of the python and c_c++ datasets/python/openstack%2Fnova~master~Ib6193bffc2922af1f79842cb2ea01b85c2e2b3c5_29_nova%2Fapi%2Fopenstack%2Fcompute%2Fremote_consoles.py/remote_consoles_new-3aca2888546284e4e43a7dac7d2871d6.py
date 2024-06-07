# Copyright 2012 OpenStack Foundation
#
#    Licensed under the Apache License, Version 2.0 (the "License"); you may
#    not use this file except in compliance with the License. You may obtain
#    a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
#    WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
#    License for the specific language governing permissions and limitations
#    under the License.

from oslo_log import log as logging
import webob

from nova.api.openstack import common
from nova.api.openstack.compute.schemas import remote_consoles
from nova.api.openstack import wsgi
from nova.api import validation
from nova.compute import api as compute
from nova import exception
from nova.policies import remote_consoles as rc_policies

LOG = logging.getLogger(__name__)


class RemoteConsolesController(wsgi.Controller):
    def __init__(self):
        super(RemoteConsolesController, self).__init__()
        self.compute_api = compute.API()
        self.handlers = {'vnc': self.compute_api.get_vnc_console,
                         'spice': self.compute_api.get_spice_console,
                         'rdp': self.compute_api.get_rdp_console,
                         'serial': self.compute_api.get_serial_console,
                         'mks': self.compute_api.get_mks_console}

    @wsgi.Controller.api_version("2.1", "2.5")
    @wsgi.expected_errors((400, 404, 409, 501))
    @wsgi.action('os-getVNCConsole')
    @validation.schema(remote_consoles.get_vnc_console)
    def get_vnc_console(self, req, id, body):
        """Get text console output."""
        context = req.environ['nova.context']
        context.can(rc_policies.BASE_POLICY_NAME)

        # If type is not supplied or unknown, get_vnc_console below will cope
        console_type = body['os-getVNCConsole'].get('type')

        instance = common.get_instance(self.compute_api, context, id)
        try:
            output = self.compute_api.get_vnc_console(context,
                                                      instance,
                                                      console_type)
        except exception.ConsoleTypeUnavailable as e:
            raise webob.exc.HTTPBadRequest(explanation=e.format_message())
        except exception.InstanceNotFound as e:
            raise webob.exc.HTTPNotFound(explanation=e.format_message())
        except exception.InstanceNotReady as e:
            raise webob.exc.HTTPConflict(explanation=e.format_message())
        except NotImplementedError:
            common.raise_feature_not_supported()

        return {'console': {'type': console_type, 'url': output['url']}}

    @wsgi.Controller.api_version("2.1", "2.5")
    @wsgi.expected_errors((400, 404, 409, 501))
    @wsgi.action('os-getSPICEConsole')
    @validation.schema(remote_consoles.get_spice_console)
    def get_spice_console(self, req, id, body):
        """Get text console output."""
        context = req.environ['nova.context']
        context.can(rc_policies.BASE_POLICY_NAME)

        # If type is not supplied or unknown, get_spice_console below will cope
        console_type = body['os-getSPICEConsole'].get('type')

        instance = common.get_instance(self.compute_api, context, id)
        try:
            output = self.compute_api.get_spice_console(context,
                                                        instance,
                                                        console_type)
        except exception.ConsoleTypeUnavailable as e:
            raise webob.exc.HTTPBadRequest(explanation=e.format_message())
        except exception.InstanceNotFound as e:
            raise webob.exc.HTTPNotFound(explanation=e.format_message())
        except exception.InstanceNotReady as e:
            raise webob.exc.HTTPConflict(explanation=e.format_message())
        except NotImplementedError:
            common.raise_feature_not_supported()

        return {'console': {'type': console_type, 'url': output['url']}}

    @wsgi.Controller.api_version("2.1", "2.5")
    @wsgi.expected_errors((400, 404, 409, 501))
    @wsgi.action('os-getRDPConsole')
    @validation.schema(remote_consoles.get_rdp_console)
    def get_rdp_console(self, req, id, body):
        """Get text console output."""
        context = req.environ['nova.context']
        context.can(rc_policies.BASE_POLICY_NAME)

        # If type is not supplied or unknown, get_rdp_console below will cope
        console_type = body['os-getRDPConsole'].get('type')

        instance = common.get_instance(self.compute_api, context, id)
        try:
            # NOTE(mikal): get_rdp_console() can raise InstanceNotFound, so
            # we still need to catch it here.
            output = self.compute_api.get_rdp_console(context,
                                                      instance,
                                                      console_type)
        except exception.ConsoleTypeUnavailable as e:
            raise webob.exc.HTTPBadRequest(explanation=e.format_message())
        except exception.InstanceNotFound as e:
            raise webob.exc.HTTPNotFound(explanation=e.format_message())
        except exception.InstanceNotReady as e:
            raise webob.exc.HTTPConflict(explanation=e.format_message())
        except NotImplementedError:
            common.raise_feature_not_supported()

        return {'console': {'type': console_type, 'url': output['url']}}

    @wsgi.Controller.api_version("2.1", "2.5")
    @wsgi.expected_errors((400, 404, 409, 501))
    @wsgi.action('os-getSerialConsole')
    @validation.schema(remote_consoles.get_serial_console)
    def get_serial_console(self, req, id, body):
        """Get connection to a serial console."""
        context = req.environ['nova.context']
        context.can(rc_policies.BASE_POLICY_NAME)

        # If type is not supplied or unknown get_serial_console below will cope
        console_type = body['os-getSerialConsole'].get('type')
        instance = common.get_instance(self.compute_api, context, id)
        try:
            output = self.compute_api.get_serial_console(context,
                                                         instance,
                                                         console_type)
        except exception.InstanceNotFound as e:
            raise webob.exc.HTTPNotFound(explanation=e.format_message())
        except exception.InstanceNotReady as e:
            raise webob.exc.HTTPConflict(explanation=e.format_message())
        except (exception.ConsoleTypeUnavailable,
                exception.ImageSerialPortNumberInvalid,
                exception.ImageSerialPortNumberExceedFlavorValue,
                exception.SocketPortRangeExhaustedException) as e:
            raise webob.exc.HTTPBadRequest(explanation=e.format_message())
        except NotImplementedError:
            common.raise_feature_not_supported()

        return {'console': {'type': console_type, 'url': output['url']}}

    def _check_proto_support_passwd(self, protocol):
        # NOTE(pandatt): if `protocol` type graphics do not support
        # setting password, UnsupportedResetVNCPassword will raise. Please see
        # https://libvirt.org/formatdomain.html#elementsGraphics, vnc/spice
        # graphics both support `passwd` option, however, the latter is not
        # fully tested for now, we will support it in future.
        if protocol not in ['vnc']:
            LOG.warning("Setting or re-setting password is not supported for "
                "the %s protocol", protocol)
            raise exception.UnsupportedResetVNCPassword(protocol=protocol)

    @wsgi.Controller.api_version("2.6")
    @wsgi.expected_errors((400, 404, 409, 501))
    @validation.schema(remote_consoles.create_v26, "2.6", "2.7")
    @validation.schema(remote_consoles.create_v28, "2.8", "2.87")
    @validation.schema(remote_consoles.create_v289, "2.89")
    def create(self, req, server_id, body):
        context = req.environ['nova.context']
        instance = common.get_instance(self.compute_api, context, server_id)
        context.can(rc_policies.BASE_POLICY_NAME,
                    target={'project_id': instance.project_id})
        protocol = body['remote_console']['protocol']
        console_type = body['remote_console']['type']
        password = body['remote_console'].get('password')

        try:
            if password:
                self._check_proto_support_passwd(protocol)
                instance.metadata['console_passwd'] = password
            handler = self.handlers.get(protocol)
            output = handler(context, instance, console_type)
            return {'remote_console': {'protocol': protocol,
                                       'type': console_type,
                                       'url': output['url']}}

        except exception.InstanceNotFound as e:
            raise webob.exc.HTTPNotFound(explanation=e.format_message())
        except exception.InstanceNotReady as e:
            raise webob.exc.HTTPConflict(explanation=e.format_message())
        except exception.InstanceInvalidState as state_error:
            common.raise_http_conflict_for_instance_invalid_state(
                state_error, 'reset_vnc_password', id)
        except (exception.ConsoleTypeInvalid,
                exception.ConsoleTypeUnavailable,
                exception.ImageSerialPortNumberInvalid,
                exception.ImageSerialPortNumberExceedFlavorValue,
                exception.SocketPortRangeExhaustedException,
                exception.UnsupportedResetVNCPassword) as e:
            raise webob.exc.HTTPBadRequest(explanation=e.format_message())
        except NotImplementedError:
            common.raise_feature_not_supported()
