import re
import unittest
from pathlib import Path
from string import Template

import yaml


FIRMWARE = Path(__file__).resolve().parents[1] / "firmware"


def load_package(path, variables=None):
    class Loader(yaml.SafeLoader):
        pass

    def include(loader, node):
        if isinstance(node, yaml.ScalarNode):
            return load_package(path.parent / loader.construct_scalar(node))
        spec = loader.construct_mapping(node, deep=True)
        return load_package(path.parent / spec["file"], spec.get("vars"))

    Loader.add_constructor("!include", include)
    Loader.add_constructor("!secret", lambda loader, node: loader.construct_scalar(node))
    source = Template(path.read_text()).safe_substitute(variables or {})
    return yaml.load(source, Loader=Loader)


def entities(config):
    result = {}
    for package in config.get("packages", {}).values():
        result.update(entities(package))
    for domain in ("climate", "select", "switch", "number", "sensor", "text_sensor", "binary_sensor", "button"):
        for entity in config.get(domain, []):
            # Multi-sensor platforms (debug) name each sub-sensor separately.
            if entity.get("platform") == "debug":
                for sub in ("free", "min_free", "loop_time", "device", "reset_reason"):
                    if sub in entity:
                        item = dict(entity[sub])
                        item["platform"] = "debug"
                        result[(domain, item["name"])] = item
                continue
            result[(domain, entity["name"])] = entity
    return result


class ProfileTests(unittest.TestCase):
    def test_tac_selection(self):
        selected = entities(load_package(FIRMWARE / "tac-12chsd.yaml"))
        self.assertNotIn(("sensor", "Outdoor temperature (raw)"), selected)
        self.assertNotIn(("switch", "PTC heater"), selected)
        self.assertTrue(selected[("number", "Generator mode")]["disabled_by_default"])
        self.assertFalse(selected[("sensor", "Outdoor temperature (estimated)")].get("disabled_by_default", False))

    def test_all_fields_covers_every_existing_definition(self):
        selected = entities(load_package(FIRMWARE / "all-fields.yaml"))
        # Include commented-out definitions, not just the TAC profile's active entities.
        source = (FIRMWARE / "tac-12chsd-extras.yaml").read_text()
        names = set(re.findall(r'entity_name: "([^"]+)"|name: "([^"]+)"', source))
        expected = {a or b for a, b in names}
        actual = {name for _, name in selected}
        self.assertTrue(expected <= actual, expected - actual)
        self.assertIn("Outdoor temperature (raw)", actual)
        optional = entities(load_package(FIRMWARE / "all-fields-extras.yaml"))
        self.assertTrue(optional)
        for key, entity in optional.items():
            with self.subTest(entity=key):
                self.assertTrue(entity.get("disabled_by_default", False))

    def test_entities_publish_only_changes(self):
        for profile in ("tac-12chsd.yaml", "all-fields.yaml"):
            for (domain, name), entity in entities(load_package(FIRMWARE / profile)).items():
                with self.subTest(profile=profile, domain=domain, entity=name):
                    if domain == "sensor":
                        self.assertEqual(entity.get("filters"), [{"delta": 0.0}])
                    elif domain in ("number", "select", "text_sensor"):
                        if entity.get("platform") == "debug":
                            continue  # Upstream debug text sensor publishes at boot only.
                        entity_id = entity["id"]
                        self.assertIn(f"id({entity_id}).has_state()", entity["lambda"])
                        self.assertIn("return {};", entity["lambda"])

    def test_module_health_diagnostics(self):
        health = [("sensor", name) for name in
                  ("Uptime", "Wi-Fi signal", "Free heap", "Minimum free heap", "Max loop time")]
        health.append(("text_sensor", "Reset reason"))
        # The everyday profile keeps periodic Wi-Fi traffic to the AC's own
        # entities: no module-health sampling, only the on-demand restart.
        tac_config = load_package(FIRMWARE / "tac-12chsd.yaml")
        tac = entities(tac_config)
        self.assertNotIn("debug", load_package(FIRMWARE / "ac-common.yaml"))
        self.assertNotIn("debug", load_package(FIRMWARE / "tac-12chsd-extras.yaml"))
        for key in health:
            with self.subTest(profile="tac-12chsd.yaml", entity=key):
                self.assertNotIn(key, tac)
        self.assertIn(("button", "Restart"), tac)
        # all-fields carries them, disabled by default like its other optional entities.
        self.assertIn("debug", load_package(FIRMWARE / "all-fields-extras.yaml"))
        all_fields = entities(load_package(FIRMWARE / "all-fields.yaml"))
        for key in health:
            with self.subTest(profile="all-fields.yaml", entity=key):
                self.assertTrue(all_fields[key]["disabled_by_default"])
        self.assertIn(("button", "Restart"), all_fields)

    def test_wpa2_policy_is_set_before_wifi_starts(self):
        from esphome.config_helpers import merge_config

        config = merge_config(load_package(FIRMWARE / "bench.yaml"),
                              load_package(FIRMWARE / "ac-common.yaml"))
        self.assertIn("<wifi_conf.h>", config["esphome"]["includes"])
        boot = config["esphome"]["on_boot"]
        self.assertIsInstance(boot, list)
        self.assertEqual({action["priority"] for action in boot}, {300, -100})
        security_boot = next(action for action in boot if action["priority"] > 250)
        self.assertIn("wifi_set_wpa_mode(WPA2_ONLY_MODE)", security_boot["then"][0]["lambda"])

    def test_shared_controls_are_identical(self):
        tac = entities(load_package(FIRMWARE / "tac-12chsd.yaml"))
        all_fields = entities(load_package(FIRMWARE / "all-fields.yaml"))
        common = entities(load_package(FIRMWARE / "ac-common.yaml"))
        for key, entity in common.items():
            with self.subTest(entity=key):
                self.assertEqual(tac[key], entity)
                self.assertEqual(all_fields[key], entity)


if __name__ == "__main__":
    unittest.main()
