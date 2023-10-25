//===- ESIManifest.td - System manifest stuff ---------------- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_ESI_MANIFEST_TD
#define CIRCT_DIALECT_ESI_MANIFEST_TD

include "circt/Dialect/ESI/ESIChannels.td"
include "circt/Dialect/HW/HWAttributesNaming.td"

include "mlir/IR/AttrTypeBase.td"

def AppIDAttr : ESI_Attr<"AppID"> {
  let summary = "An application relevant instance identifier";
  let description = [{
    Identifies an instance which is visible through multiple hierarchy levels.
    Indended to make locating an instance easier in the instance hierarchy.
  }];

  let parameters = (ins "StringAttr":$name,
                        OptionalParameter<"std::optional<uint64_t>">:$index);
  let mnemonic = "appid";
  let assemblyFormat = [{
    `<` $name (`[` $index^ `]`)? `>`
  }];

  let extraClassDeclaration = [{
    static constexpr StringRef AppIDAttrName = "esi.appid";
  }];
}

def AppIDArrayAttr :
  TypedArrayAttrBase<AppIDAttr, "Array of AppIDs">;

def AppIDPathAttr : ESI_Attr<"AppIDPath"> {
  let summary = "An application-specific hierarchical path through a design";
  let description = [{
    A list of AppID components which specifies a specific dynamic instance
    in the design.
  }];

  let parameters = (ins "FlatSymbolRefAttr":$root,
                        ArrayRefParameter<"AppIDAttr">:$path);
  let mnemonic = "appid_path";
  let assemblyFormat = [{
    `<` $root `[` $path `]` `>`
  }];
}

def BundleDirection : I32EnumAttr<"BundleDirection",
  "Direction of original request", [
    I32EnumAttrCase<"toServer", 1>,
    I32EnumAttrCase<"toClient", 2>,
  ]> {
  let cppNamespace = "::circt::esi";
}

def ServiceRequestRecordOp : ESI_Op<"esi.manifest.req", [
        DeclareOpInterfaceMethods<HasAppIDOpInterface>]> {
  let summary = "Record of a service request";
  let description = [{
    A record of a service request, including the requestor, the service
    requested, and the parameters of the request. Emitted before connecting the
    service to preserve metadata about the original request.
  }];

  let arguments = (ins AppIDAttr:$requestor,
                       InnerRefAttr:$servicePort,
                       BundleDirection:$direction,
                       TypeAttrOf<ChannelBundleType>:$bundleType);

  let assemblyFormat = [{
    qualified($requestor) `,` $servicePort `,` $direction `,` $bundleType
    attr-dict
  }];

  let extraClassDeclaration = [{
    AppIDAttr getAppID() {
      return getRequestor();
    }
  }];
}

def ServiceImplRecordOp : ESI_Op<"esi.manifest.service_impl", [NoTerminator]> {
  let summary = "Record of a service implementation";
  let description = [{
    A record of a service implementation. Optionally emitted by the service
    implementation. Contains information necessary to connect to the service and
    service clients.
  }];

  let arguments = (ins OptionalAttr<FlatSymbolRefAttr>:$service,
                       StrAttr:$serviceImplName,
                       DictionaryAttr:$implDetails);
  let regions = (region SizedRegion<1>:$reqDetails);
  let assemblyFormat = [{
    (`svc` $service^)? `by` $serviceImplName `with` $implDetails
    attr-dict-with-keyword $reqDetails
  }];
}

def ServiceImplClientRecordOp : ESI_Op<"esi.manifest.impl_conn", [
      HasParent<"ServiceImplRecordOp">]> {
  let summary = "Details of a service implementation client connection";
  let description = [{
    A record containing all the necessary details of how to connect to a client
    which the parent service record is servicing.
  }];

  let arguments = (ins AppIDArrayAttr:$relAppIDPath,
                       InnerRefAttr:$servicePort,
                       TypeAttrOf<ChannelBundleType>:$bundleType,
                       DictionaryAttr:$implDetails);
  let assemblyFormat = [{
    $relAppIDPath `req` $servicePort `(` $bundleType `)`
      `with` $implDetails attr-dict
  }];
}

#endif // CIRCT_DIALECT_ESI_MANIFEST_TD