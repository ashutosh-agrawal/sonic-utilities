from natsort import natsorted
from swsscommon.swsscommon import ConfigDBConnector, SonicV2Connector


class OperationError(ValueError):
    pass


FEATURE_FIELDS = (
    "state",
    "auto_restart",
    "system_state",
    "update_time",
    "container_id",
    "container_version",
    "set_owner",
    "current_owner",
    "remote_state",
)
COUNTER_PACKETS_ATTR = "SAI_ACL_COUNTER_ATTR_PACKETS"
COUNTER_BYTES_ATTR = "SAI_ACL_COUNTER_ATTR_BYTES"
MAX_FILTER_VALUES = 256
MAX_FILTER_LENGTH = 256


def feature_status(arguments):
    _require_keys(arguments, {"feature_name"})
    feature_name = arguments.get("feature_name")
    if feature_name is not None and (not isinstance(feature_name, str) or not feature_name):
        raise OperationError("feature_name must be a non-empty string")
    if feature_name is not None and len(feature_name) > MAX_FILTER_LENGTH:
        raise OperationError("feature_name is too long")

    config_db = ConfigDBConnector(use_unix_socket_path=True)
    state_db = SonicV2Connector(use_unix_socket_path=True)
    try:
        config_db.connect()
        state_db.connect(state_db.STATE_DB)

        configured = config_db.get_table("FEATURE")
        if feature_name is not None:
            if feature_name not in configured:
                raise OperationError("Can not find feature {}".format(feature_name))
            names = [feature_name]
        else:
            names = natsorted(configured.keys())

        rows = []
        for name in names:
            data = state_db.get_all(state_db.STATE_DB, "FEATURE|{}".format(name)) or {}
            data.update(configured[name])
            rows.append({
                "name": name,
                "fields": {field: data[field] for field in FEATURE_FIELDS if field in data},
            })
        return {"rows": rows}
    finally:
        config_db.close()
        state_db.close()


def acl_counters(arguments):
    _require_keys(arguments, {"rules", "tables"})
    rules = _string_list(arguments.get("rules"), "rules")
    tables = _string_list(arguments.get("tables"), "tables")

    config_db = ConfigDBConnector(use_unix_socket_path=True)
    counters_db = SonicV2Connector(use_unix_socket_path=True)
    try:
        config_db.connect()
        counters_db.connect(counters_db.COUNTERS_DB)

        acl_tables = config_db.get_table("ACL_TABLE")
        acl_rules = config_db.get_table("ACL_RULE")
        total_tables = len(acl_tables)
        total_rules = len(acl_rules)

        if tables:
            acl_rules = {
                (table, rule): content
                for (table, rule), content in acl_rules.items()
                if table in tables
            }
        if rules:
            acl_rules = {
                (table, rule): content
                for (table, rule), content in acl_rules.items()
                if rule in rules
            }

        rule_to_counter = {}
        if counters_db.exists(counters_db.COUNTERS_DB, "ACL_COUNTER_RULE_MAP"):
            rule_to_counter = counters_db.get_all(
                counters_db.COUNTERS_DB, "ACL_COUNTER_RULE_MAP"
            )
        separator = counters_db.get_db_separator(counters_db.COUNTERS_DB)

        rows = []
        for (table, rule), content in acl_rules.items():
            priority = -1
            for field, value in content.items():
                if field.upper() == "PRIORITY":
                    priority = value
                    break

            counters = {}
            counter_oid = rule_to_counter.get(table + separator + rule)
            if counter_oid:
                counter_key = "COUNTERS{}{}".format(separator, counter_oid)
                values = counters_db.get_all(counters_db.COUNTERS_DB, counter_key) or {}
                for field in (COUNTER_PACKETS_ATTR, COUNTER_BYTES_ATTR):
                    if field in values:
                        counters[field] = values[field]

            rows.append({
                "table": table,
                "rule": rule,
                "priority": priority,
                "counters": counters,
            })

        return {
            "total_tables": total_tables,
            "total_rules": total_rules,
            "rows": rows,
        }
    finally:
        config_db.close()
        counters_db.close()


OPERATIONS = {
    "feature_status": feature_status,
    "acl_counters": acl_counters,
}


def dispatch(operation, arguments):
    if not isinstance(operation, str) or operation not in OPERATIONS:
        raise OperationError("operation denied")
    if not isinstance(arguments, dict):
        raise OperationError("arguments must be an object")
    return OPERATIONS[operation](arguments)


def _require_keys(arguments, allowed):
    unexpected = set(arguments) - allowed
    if unexpected:
        raise OperationError("unexpected argument")


def _string_list(value, name):
    if value is None:
        return []
    if not isinstance(value, list) or len(value) > MAX_FILTER_VALUES:
        raise OperationError("{} must be a bounded list".format(name))
    for item in value:
        if not isinstance(item, str) or not item or len(item) > MAX_FILTER_LENGTH:
            raise OperationError("{} contains an invalid value".format(name))
    return value
