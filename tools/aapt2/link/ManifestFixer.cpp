/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "link/ManifestFixer.h"

#include <unordered_set>

#include "android-base/logging.h"

#include "ResourceUtils.h"
#include "util/Util.h"
#include "xml/XmlActionExecutor.h"
#include "xml/XmlDom.h"

namespace aapt {

/**
 * This is how PackageManager builds class names from AndroidManifest.xml
 * entries.
 */
static bool NameIsJavaClassName(xml::Element* el, xml::Attribute* attr,
                                SourcePathDiagnostics* diag) {
  // We allow unqualified class names (ie: .HelloActivity)
  // Since we don't know the package name, we can just make a fake one here and
  // the test will be identical as long as the real package name is valid too.
  Maybe<std::string> fully_qualified_class_name =
      util::GetFullyQualifiedClassName("a", attr->value);

  StringPiece qualified_class_name = fully_qualified_class_name
                                         ? fully_qualified_class_name.value()
                                         : attr->value;

  if (!util::IsJavaClassName(qualified_class_name)) {
    diag->Error(DiagMessage(el->line_number)
                << "attribute 'android:name' in <" << el->name
                << "> tag must be a valid Java class name");
    return false;
  }
  return true;
}

static bool OptionalNameIsJavaClassName(xml::Element* el,
                                        SourcePathDiagnostics* diag) {
  if (xml::Attribute* attr = el->FindAttribute(xml::kSchemaAndroid, "name")) {
    return NameIsJavaClassName(el, attr, diag);
  }
  return true;
}

static bool RequiredNameIsJavaClassName(xml::Element* el,
                                        SourcePathDiagnostics* diag) {
  if (xml::Attribute* attr = el->FindAttribute(xml::kSchemaAndroid, "name")) {
    return NameIsJavaClassName(el, attr, diag);
  }
  diag->Error(DiagMessage(el->line_number)
              << "<" << el->name << "> is missing attribute 'android:name'");
  return false;
}

static bool VerifyManifest(xml::Element* el, SourcePathDiagnostics* diag) {
  xml::Attribute* attr = el->FindAttribute({}, "package");
  if (!attr) {
    diag->Error(DiagMessage(el->line_number)
                << "<manifest> tag is missing 'package' attribute");
    return false;
  } else if (ResourceUtils::IsReference(attr->value)) {
    diag->Error(
        DiagMessage(el->line_number)
        << "attribute 'package' in <manifest> tag must not be a reference");
    return false;
  } else if (!util::IsJavaPackageName(attr->value)) {
    diag->Error(DiagMessage(el->line_number)
                << "attribute 'package' in <manifest> tag is not a valid Java "
                   "package name: '"
                << attr->value << "'");
    return false;
  }
  return true;
}

/**
 * The coreApp attribute in <manifest> is not a regular AAPT attribute, so type
 * checking on it is manual.
 */
static bool FixCoreAppAttribute(xml::Element* el, SourcePathDiagnostics* diag) {
  if (xml::Attribute* attr = el->FindAttribute("", "coreApp")) {
    std::unique_ptr<BinaryPrimitive> result =
        ResourceUtils::TryParseBool(attr->value);
    if (!result) {
      diag->Error(DiagMessage(el->line_number)
                  << "attribute coreApp must be a boolean");
      return false;
    }
    attr->compiled_value = std::move(result);
  }
  return true;
}

bool ManifestFixer::BuildRules(xml::XmlActionExecutor* executor,
                               IDiagnostics* diag) {
  // First verify some options.
  if (options_.rename_manifest_package) {
    if (!util::IsJavaPackageName(options_.rename_manifest_package.value())) {
      diag->Error(DiagMessage() << "invalid manifest package override '"
                                << options_.rename_manifest_package.value()
                                << "'");
      return false;
    }
  }

  if (options_.rename_instrumentation_target_package) {
    if (!util::IsJavaPackageName(
            options_.rename_instrumentation_target_package.value())) {
      diag->Error(DiagMessage()
                  << "invalid instrumentation target package override '"
                  << options_.rename_instrumentation_target_package.value()
                  << "'");
      return false;
    }
  }

  // Common intent-filter actions.
  xml::XmlNodeAction intent_filter_action;
  intent_filter_action["action"];
  intent_filter_action["category"];
  intent_filter_action["data"];

  // Common meta-data actions.
  xml::XmlNodeAction meta_data_action;

  // Manifest actions.
  xml::XmlNodeAction& manifest_action = (*executor)["manifest"];
  manifest_action.Action(VerifyManifest);
  manifest_action.Action(FixCoreAppAttribute);
  manifest_action.Action([&](xml::Element* el) -> bool {
    if (options_.version_name_default) {
      if (el->FindAttribute(xml::kSchemaAndroid, "versionName") == nullptr) {
        el->attributes.push_back(
            xml::Attribute{xml::kSchemaAndroid, "versionName",
                           options_.version_name_default.value()});
      }
    }

    if (options_.version_code_default) {
      if (el->FindAttribute(xml::kSchemaAndroid, "versionCode") == nullptr) {
        el->attributes.push_back(
            xml::Attribute{xml::kSchemaAndroid, "versionCode",
                           options_.version_code_default.value()});
      }
    }
    return true;
  });

  // Meta tags.
  manifest_action["eat-comment"];

  // Uses-sdk actions.
  manifest_action["uses-sdk"].Action([&](xml::Element* el) -> bool {
    if (options_.min_sdk_version_default &&
        el->FindAttribute(xml::kSchemaAndroid, "minSdkVersion") == nullptr) {
      // There was no minSdkVersion defined and we have a default to assign.
      el->attributes.push_back(
          xml::Attribute{xml::kSchemaAndroid, "minSdkVersion",
                         options_.min_sdk_version_default.value()});
    }

    if (options_.target_sdk_version_default &&
        el->FindAttribute(xml::kSchemaAndroid, "targetSdkVersion") == nullptr) {
      // There was no targetSdkVersion defined and we have a default to assign.
      el->attributes.push_back(
          xml::Attribute{xml::kSchemaAndroid, "targetSdkVersion",
                         options_.target_sdk_version_default.value()});
    }
    return true;
  });

  // Instrumentation actions.
  manifest_action["instrumentation"].Action([&](xml::Element* el) -> bool {
    if (!options_.rename_instrumentation_target_package) {
      return true;
    }

    if (xml::Attribute* attr =
            el->FindAttribute(xml::kSchemaAndroid, "targetPackage")) {
      attr->value = options_.rename_instrumentation_target_package.value();
    }
    return true;
  });

  manifest_action["original-package"];
  manifest_action["protected-broadcast"];
  manifest_action["uses-permission"];
  manifest_action["permission"];
  manifest_action["permission-tree"];
  manifest_action["permission-group"];

  manifest_action["uses-configuration"];
  manifest_action["uses-feature"];
  manifest_action["supports-screens"];

  manifest_action["compatible-screens"];
  manifest_action["compatible-screens"]["screen"];

  manifest_action["supports-gl-texture"];

  // Application actions.
  xml::XmlNodeAction& application_action = manifest_action["application"];
  application_action.Action(OptionalNameIsJavaClassName);

  // Uses library actions.
  application_action["uses-library"];

  // Meta-data.
  application_action["meta-data"] = meta_data_action;

  // Activity actions.
  application_action["activity"].Action(RequiredNameIsJavaClassName);
  application_action["activity"]["intent-filter"] = intent_filter_action;
  application_action["activity"]["meta-data"] = meta_data_action;

  // Activity alias actions.
  application_action["activity-alias"]["intent-filter"] = intent_filter_action;
  application_action["activity-alias"]["meta-data"] = meta_data_action;

  // Service actions.
  application_action["service"].Action(RequiredNameIsJavaClassName);
  application_action["service"]["intent-filter"] = intent_filter_action;
  application_action["service"]["meta-data"] = meta_data_action;

  // Receiver actions.
  application_action["receiver"].Action(RequiredNameIsJavaClassName);
  application_action["receiver"]["intent-filter"] = intent_filter_action;
  application_action["receiver"]["meta-data"] = meta_data_action;

  // Provider actions.
  application_action["provider"].Action(RequiredNameIsJavaClassName);
  application_action["provider"]["intent-filter"] = intent_filter_action;
  application_action["provider"]["meta-data"] = meta_data_action;
  application_action["provider"]["grant-uri-permissions"];
  application_action["provider"]["path-permissions"];

  return true;
}

class FullyQualifiedClassNameVisitor : public xml::Visitor {
 public:
  using xml::Visitor::Visit;

  explicit FullyQualifiedClassNameVisitor(const StringPiece& package)
      : package_(package) {}

  void Visit(xml::Element* el) override {
    for (xml::Attribute& attr : el->attributes) {
      if (attr.namespace_uri == xml::kSchemaAndroid &&
          class_attributes_.find(attr.name) != class_attributes_.end()) {
        if (Maybe<std::string> new_value =
                util::GetFullyQualifiedClassName(package_, attr.value)) {
          attr.value = std::move(new_value.value());
        }
      }
    }

    // Super implementation to iterate over the children.
    xml::Visitor::Visit(el);
  }

 private:
  StringPiece package_;
  std::unordered_set<StringPiece> class_attributes_ = {"name"};
};

static bool RenameManifestPackage(const StringPiece& package_override,
                                  xml::Element* manifest_el) {
  xml::Attribute* attr = manifest_el->FindAttribute({}, "package");

  // We've already verified that the manifest element is present, with a package
  // name specified.
  CHECK(attr != nullptr);

  std::string original_package = std::move(attr->value);
  attr->value = package_override.ToString();

  FullyQualifiedClassNameVisitor visitor(original_package);
  manifest_el->Accept(&visitor);
  return true;
}

bool ManifestFixer::Consume(IAaptContext* context, xml::XmlResource* doc) {
  xml::Element* root = xml::FindRootElement(doc->root.get());
  if (!root || !root->namespace_uri.empty() || root->name != "manifest") {
    context->GetDiagnostics()->Error(DiagMessage(doc->file.source)
                                     << "root tag must be <manifest>");
    return false;
  }

  if ((options_.min_sdk_version_default ||
       options_.target_sdk_version_default) &&
      root->FindChild({}, "uses-sdk") == nullptr) {
    // Auto insert a <uses-sdk> element.
    std::unique_ptr<xml::Element> uses_sdk = util::make_unique<xml::Element>();
    uses_sdk->name = "uses-sdk";
    root->AddChild(std::move(uses_sdk));
  }

  xml::XmlActionExecutor executor;
  if (!BuildRules(&executor, context->GetDiagnostics())) {
    return false;
  }

  if (!executor.Execute(xml::XmlActionExecutorPolicy::kWhitelist,
                        context->GetDiagnostics(), doc)) {
    return false;
  }

  if (options_.rename_manifest_package) {
    // Rename manifest package outside of the XmlActionExecutor.
    // We need to extract the old package name and FullyQualify all class names.
    if (!RenameManifestPackage(options_.rename_manifest_package.value(),
                               root)) {
      return false;
    }
  }
  return true;
}

}  // namespace aapt
