#    Copyright 2013 Rackspace Hosting.
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

from oslo_db import exception as db_exc

from nova import db
from nova.db.sqlalchemy import api as db_api
from nova.db.sqlalchemy import api_models
from nova import exception
from nova.objects import base
from nova.objects import fields
from nova import quota


def ids_from_instance(context, instance):
    if (context.is_admin and
            context.project_id != instance['project_id']):
        project_id = instance['project_id']
    else:
        project_id = context.project_id
    if context.user_id != instance['user_id']:
        user_id = instance['user_id']
    else:
        user_id = context.user_id
    return project_id, user_id


# TODO(lyj): This method needs to be cleaned up once the
# ids_from_instance helper method is renamed or some common
# method is added for objects.quotas.
def ids_from_security_group(context, security_group):
    return ids_from_instance(context, security_group)


# TODO(PhilD): This method needs to be cleaned up once the
# ids_from_instance helper method is renamed or some common
# method is added for objects.quotas.
def ids_from_server_group(context, server_group):
    return ids_from_instance(context, server_group)


@base.NovaObjectRegistry.register
class Quotas(base.NovaObject):
    # Version 1.0: initial version
    # Version 1.1: Added create_limit() and update_limit()
    # Version 1.2: Added limit_check() and count()
    # Version 1.3: Added get(), get_all(), get_all_by_project(),
    #              get_all_by_project_and_user(), destroy_all_by_project(),
    #              destroy_all_by_project_and_user(), get_class(),
    #              get_default_class(), get_all_class_by_name(),
    #              create_class(), update_class()
    VERSION = '1.3'

    fields = {
        'reservations': fields.ListOfStringsField(nullable=True),
        'project_id': fields.StringField(nullable=True),
        'user_id': fields.StringField(nullable=True),
    }

    def __init__(self, *args, **kwargs):
        super(Quotas, self).__init__(*args, **kwargs)
        # Set up defaults.
        self.reservations = []
        self.project_id = None
        self.user_id = None
        self.obj_reset_changes()

    @staticmethod
    @db_api.api_context_manager.reader
    def _get_from_db(context, project_id, resource, user_id=None):
        model = api_models.ProjectUserQuota if user_id else api_models.Quota
        query = context.session.query(model).\
                        filter_by(project_id=project_id).\
                        filter_by(resource=resource)
        if user_id:
            query = query.filter_by(user_id=user_id)
        result = query.first()
        if not result:
            if user_id:
                raise exception.ProjectUserQuotaNotFound(project_id=project_id,
                                                         user_id=user_id)
            else:
                raise exception.ProjectQuotaNotFound(project_id=project_id)
        return result

    @staticmethod
    @db_api.api_context_manager.reader
    def _get_all_from_db(context, project_id):
        return context.session.query(api_models.ProjectUserQuota).\
                        filter_by(project_id=project_id).\
                        all()

    @staticmethod
    @db_api.api_context_manager.reader
    def _get_all_from_db_by_project(context, project_id):
        # by_project refers to the result that is a dict with a project_id key
        rows = context.session.query(api_models.Quota).\
                        filter_by(project_id=project_id).\
                        all()
        result = {'project_id': project_id}
        for row in rows:
            result[row.resource] = row.hard_limit
        return result

    @staticmethod
    @db_api.api_context_manager.reader
    def _get_all_from_db_by_project_and_user(context, project_id, user_id):
        # by_project_and_user refers to the result that is a dict with
        # project_id and user_id keys
        columns = (api_models.ProjectUserQuota.resource,
                   api_models.ProjectUserQuota.hard_limit)
        user_quotas = context.session.query(*columns).\
                        filter_by(project_id=project_id).\
                        filter_by(user_id=user_id).\
                        all()
        result = {'project_id': project_id, 'user_id': user_id}
        for user_quota in user_quotas:
            result[user_quota.resource] = user_quota.hard_limit
        return result

    @staticmethod
    @db_api.api_context_manager.writer
    def _destroy_all_in_db_by_project(context, project_id):
        per_project = context.session.query(api_models.Quota).\
                            filter_by(project_id=project_id).\
                            delete(synchronize_session=False)
        per_user = context.session.query(api_models.ProjectUserQuota).\
                            filter_by(project_id=project_id).\
                            delete(synchronize_session=False)
        if not per_project and not per_user:
            raise exception.ProjectQuotaNotFound(project_id=project_id)

    @staticmethod
    @db_api.api_context_manager.writer
    def _destroy_all_in_db_by_project_and_user(context, project_id, user_id):
        result = context.session.query(api_models.ProjectUserQuota).\
                        filter_by(project_id=project_id).\
                        filter_by(user_id=user_id).\
                        delete(synchronize_session=False)
        if not result:
            raise exception.ProjectUserQuotaNotFound(project_id=project_id,
                                                     user_id=user_id)

    @staticmethod
    @db_api.api_context_manager.reader
    def _get_class_from_db(context, class_name, resource):
        result = context.session.query(api_models.QuotaClass).\
                        filter_by(class_name=class_name).\
                        filter_by(resource=resource).\
                        first()
        if not result:
            raise exception.QuotaClassNotFound(class_name=class_name)
        return result

    @staticmethod
    @db_api.api_context_manager.reader
    def _get_all_class_from_db_by_name(context, class_name):
        # by_name refers to the result that is a dict with a class_name key
        rows = context.session.query(api_models.QuotaClass).\
                        filter_by(class_name=class_name).\
                        all()
        result = {'class_name': class_name}
        for row in rows:
            result[row.resource] = row.hard_limit
        return result

    @staticmethod
    @db_api.api_context_manager.writer
    def _create_limit_in_db(context, project_id, resource, limit,
                            user_id=None):
        # TODO(melwitt): We won't need PER_PROJECT_QUOTAS after nova-network
        # is removed.
        per_user = user_id and resource not in db_api.PER_PROJECT_QUOTAS
        quota_ref = (api_models.ProjectUserQuota() if per_user
                     else api_models.Quota())
        if per_user:
            quota_ref.user_id = user_id
        quota_ref.project_id = project_id
        quota_ref.resource = resource
        quota_ref.hard_limit = limit
        try:
            quota_ref.save(context.session)
        except db_exc.DBDuplicateEntry:
            raise exception.QuotaExists(project_id=project_id,
                                        resource=resource)
        return quota_ref

    @staticmethod
    @db_api.api_context_manager.writer
    def _update_limit_in_db(context, project_id, resource, limit,
                            user_id=None):
        # TODO(melwitt): We won't need PER_PROJECT_QUOTAS after nova-network
        # is removed.
        per_user = user_id and resource not in db_api.PER_PROJECT_QUOTAS
        model = api_models.ProjectUserQuota if per_user else api_models.Quota
        query = context.session.query(model).\
                        filter_by(project_id=project_id).\
                        filter_by(resource=resource)
        if per_user:
            query = query.filter_by(user_id=user_id)

        result = query.update({'hard_limit': limit})
        if not result:
            if per_user:
                raise exception.ProjectUserQuotaNotFound(project_id=project_id,
                                                         user_id=user_id)
            else:
                raise exception.ProjectQuotaNotFound(project_id=project_id)

    @staticmethod
    @db_api.api_context_manager.writer
    def _create_class_in_db(context, class_name, resource, limit):
        quota_class_ref = api_models.QuotaClass()
        quota_class_ref.class_name = class_name
        quota_class_ref.resource = resource
        quota_class_ref.hard_limit = limit
        quota_class_ref.save(context.session)
        return quota_class_ref

    @staticmethod
    @db_api.api_context_manager.writer
    def _update_class_in_db(context, class_name, resource, limit):
        result = context.session.query(api_models.QuotaClass).\
                        filter_by(class_name=class_name).\
                        filter_by(resource=resource).\
                        update({'hard_limit': limit})
        if not result:
            raise exception.QuotaClassNotFound(class_name=class_name)

    @classmethod
    def from_reservations(cls, context, reservations, instance=None):
        """Transitional for compatibility."""
        if instance is None:
            project_id = None
            user_id = None
        else:
            project_id, user_id = ids_from_instance(context, instance)
        quotas = cls()
        quotas._context = context
        quotas.reservations = reservations
        quotas.project_id = project_id
        quotas.user_id = user_id
        quotas.obj_reset_changes()
        return quotas

    @base.remotable
    def reserve(self, expire=None, project_id=None, user_id=None,
                **deltas):
        reservations = quota.QUOTAS.reserve(self._context, expire=expire,
                                            project_id=project_id,
                                            user_id=user_id,
                                            **deltas)
        self.reservations = reservations
        self.project_id = project_id
        self.user_id = user_id
        self.obj_reset_changes()

    @base.remotable
    def commit(self):
        if not self.reservations:
            return
        quota.QUOTAS.commit(self._context, self.reservations,
                            project_id=self.project_id,
                            user_id=self.user_id)
        self.reservations = None
        self.obj_reset_changes()

    @base.remotable
    def rollback(self):
        """Rollback quotas."""
        if not self.reservations:
            return
        quota.QUOTAS.rollback(self._context, self.reservations,
                              project_id=self.project_id,
                              user_id=self.user_id)
        self.reservations = None
        self.obj_reset_changes()

    @base.remotable_classmethod
    def limit_check(cls, context, project_id=None, user_id=None, **values):
        """Check quota limits."""
        return quota.QUOTAS.limit_check(
            context, project_id=project_id, user_id=user_id, **values)

    @base.remotable_classmethod
    def count(cls, context, resource, *args, **kwargs):
        """Count a resource."""
        return quota.QUOTAS.count(
            context, resource, *args, **kwargs)

    @base.remotable_classmethod
    def create_limit(cls, context, project_id, resource, limit, user_id=None):
        # NOTE(danms,comstud): Quotas likely needs an overhaul and currently
        # doesn't map very well to objects. Since there is quite a bit of
        # logic in the db api layer for this, just duplicate it for now.
        cls._create_limit_in_db(context, project_id, resource, limit,
                                user_id=user_id)

    @base.remotable_classmethod
    def update_limit(cls, context, project_id, resource, limit, user_id=None):
        # NOTE(danms,comstud): Quotas likely needs an overhaul and currently
        # doesn't map very well to objects. Since there is quite a bit of
        # logic in the db api layer for this, just duplicate it for now.
        try:
            cls._update_limit_in_db(context, project_id, resource, limit,
                                    user_id=user_id)
        except exception.QuotaNotFound:
            db.quota_update(context, project_id, resource, limit,
                            user_id=user_id)

    @base.remotable_classmethod
    def get(cls, context, project_id, resource, user_id=None):
        try:
            quota = cls._get_from_db(context, project_id, resource,
                                     user_id=user_id)
        except exception.QuotaNotFound:
            quota = db.quota_get(context, project_id, resource,
                                 user_id=user_id)
        return quota

    @base.remotable_classmethod
    def get_all(cls, context, project_id):
        api_db_quotas = cls._get_all_from_db(context, project_id)
        main_db_quotas = db.quota_get_all(context, project_id)
        return api_db_quotas + main_db_quotas

    @base.remotable_classmethod
    def get_all_by_project(cls, context, project_id):
        api_db_quotas_dict = cls._get_all_from_db_by_project(context,
                                                             project_id)
        main_db_quotas_dict = db.quota_get_all_by_project(context, project_id)
        # If any keys are duplicated, favor the API key during merge.
        for k, v in api_db_quotas_dict.items():
            main_db_quotas_dict[k] = v
        return main_db_quotas_dict

    @base.remotable_classmethod
    def get_all_by_project_and_user(cls, context, project_id, user_id):
        api_db_quotas_dict = cls._get_all_from_db_by_project_and_user(
                context, project_id, user_id)
        main_db_quotas_dict = db.quota_get_all_by_project_and_user(
                context, project_id, user_id)
        # If any keys are duplicated, favor the API key during merge.
        for k, v in api_db_quotas_dict.items():
            main_db_quotas_dict[k] = v
        return main_db_quotas_dict

    @base.remotable_classmethod
    def destroy_all_by_project(cls, context, project_id):
        try:
            cls._destroy_all_in_db_by_project(context, project_id)
        except exception.ProjectQuotaNotFound:
            db.quota_destroy_all_by_project(context, project_id)

    @base.remotable_classmethod
    def destroy_all_by_project_and_user(cls, context, project_id, user_id):
        try:
            cls._destroy_all_in_db_by_project_and_user(context, project_id,
                                                       user_id)
        except exception.ProjectUserQuotaNotFound:
            db.quota_destroy_all_by_project_and_user(context, project_id,
                                                     user_id)

    @base.remotable_classmethod
    def get_class(cls, context, class_name, resource):
        try:
            qclass = cls._get_class_from_db(context, class_name, resource)
        except exception.QuotaClassNotFound:
            qclass = db.quota_class_get(context, class_name, resource)
        return qclass

    @base.remotable_classmethod
    def get_default_class(cls, context):
        try:
            qclass = cls._get_all_class_from_db_by_name(
                    context, db_api._DEFAULT_QUOTA_NAME)
        except exception.QuotaClassNotFound:
            qclass = db.quota_class_get_default(context)
        return qclass

    @base.remotable_classmethod
    def get_all_class_by_name(cls, context, class_name):
        api_db_quotas_dict = cls._get_all_class_from_db_by_name(context,
                                                                class_name)
        main_db_quotas_dict = db.quota_class_get_all_by_name(context,
                                                             class_name)
        # If any keys are duplicated, favor the API key during merge.
        for k, v in api_db_quotas_dict.items():
            main_db_quotas_dict[k] = v
        return main_db_quotas_dict

    @base.remotable_classmethod
    def create_class(cls, context, class_name, resource, limit):
        cls._create_class_in_db(context, class_name, resource, limit)

    @base.remotable_classmethod
    def update_class(cls, context, class_name, resource, limit):
        try:
            cls._update_class_in_db(context, class_name, resource, limit)
        except exception.QuotaClassNotFound:
            db.quota_class_update(context, class_name, resource, limit)


@base.NovaObjectRegistry.register
class QuotasNoOp(Quotas):
    def reserve(context, expire=None, project_id=None, user_id=None,
                **deltas):
        pass

    def commit(self, context=None):
        pass

    def rollback(self, context=None):
        pass
