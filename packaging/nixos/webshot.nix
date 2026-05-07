{
  config,
  lib,
  pkgs,
  ...
}: let
  cfg = config.services.webshot;
  configVarsFormat = pkgs.formats.yaml {};
  optionalValue = name: value:
    lib.optionalAttrs (value != null) {
      ${name} = value;
    };
  serviceUnit =
    if cfg.s3Mode == "local"
    then "webshot-local-s3.service"
    else "webshot.service";
  configVars =
    optionalValue "pg_mode" cfg.pgMode
    // optionalValue "s3_mode" cfg.s3Mode
    // optionalValue "s3_bucket" cfg.s3Bucket
    // optionalValue "public_base_url" cfg.publicBaseUrl
    // lib.optionalAttrs (cfg.secdistPath != null) {
      secdist_path = "/run/credentials/${serviceUnit}/secdist.json";
    }
    // optionalValue "allowlist_only" cfg.allowlistOnly
    // optionalValue "https_only" cfg.httpsOnly
    // optionalValue "client_ip_source" cfg.clientIpSource
    // optionalValue "client_ip_header_name" cfg.clientIpHeaderName
    // lib.optionalAttrs (cfg.pgMode == "external") (
      optionalValue "pg_capture_meta_db_dsn" cfg.pgCaptureMetaDbDsn
      // optionalValue "pg_shared_state_db_dsn" cfg.pgSharedStateDbDsn
    )
    // lib.optionalAttrs (cfg.s3Mode == "external") (
      optionalValue "s3_endpoint" cfg.s3Endpoint
      // optionalValue "s3_region" cfg.s3Region
      // optionalValue "s3_use_sts" cfg.s3UseSts
    )
    // lib.optionalAttrs (cfg.s3Mode == "external" && cfg.s3UseSts == true) (
      optionalValue "s3_credentials_endpoint" cfg.s3CredentialsEndpoint
    );
in {
  options.services.webshot = {
    enable = lib.mkEnableOption "webshot";

    package = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = null;
      description = ''
        The webshot package that provides the service unit.
      '';
    };

    pgMode = lib.mkOption {
      type = lib.types.nullOr (lib.types.enum ["local" "external"]);
      default = null;
      description = "Whether webshot manages PostgreSQL locally or connects to an external PostgreSQL.";
    };

    pgCaptureMetaDbDsn = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = "External PostgreSQL DSN for the capture metadata database.";
    };

    pgSharedStateDbDsn = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = "External PostgreSQL DSN for the shared state database.";
    };

    s3Mode = lib.mkOption {
      type = lib.types.nullOr (lib.types.enum ["local" "external"]);
      default = null;
      description = "Whether webshot stores captures in local SeaweedFS or external S3.";
    };

    s3Bucket = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = "S3 bucket name used for capture objects.";
    };

    s3Endpoint = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = "External S3 endpoint URL.";
    };

    s3Region = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = "External S3 region.";
    };

    s3UseSts = lib.mkOption {
      type = lib.types.nullOr lib.types.bool;
      default = null;
      description = "Whether to fetch temporary S3 credentials from STS in external S3 mode.";
    };

    s3CredentialsEndpoint = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = "STS endpoint used when s3UseSts is enabled.";
    };

    publicBaseUrl = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = "Externally visible base URL used to build direct capture storage URLs.";
    };

    secdistPath = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = "Path to the webshotd secdist JSON file.";
    };

    seaweedfsS3ConfigPath = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = "Path to the SeaweedFS S3 config JSON file (required when s3Mode is local).";
    };

    allowlistOnly = lib.mkOption {
      type = lib.types.nullOr lib.types.bool;
      default = null;
      description = "Whether captures are restricted to allowlisted links.";
    };

    httpsOnly = lib.mkOption {
      type = lib.types.nullOr lib.types.bool;
      default = null;
      description = "Whether capture links must use HTTPS.";
    };

    clientIpSource = lib.mkOption {
      type = lib.types.nullOr (lib.types.enum ["peer" "trusted_header"]);
      default = null;
      description = "Source of the client IP used by webshot.";
    };

    clientIpHeaderName = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = "Trusted header name used when clientIpSource is trusted_header.";
    };
  };

  config = lib.mkIf cfg.enable {
    systemd.packages = lib.optional (cfg.package != null) cfg.package;

    environment.etc."webshot/config_vars.yaml".source =
      configVarsFormat.generate "webshot-config-vars.yaml" configVars;

    systemd.targets.multi-user.wants = lib.optional (cfg.package != null) serviceUnit;

    systemd.services.${serviceUnit}.serviceConfig.LoadCredential = lib.mkIf (cfg.package != null) (
      lib.optional (cfg.secdistPath != null) "secdist.json:${cfg.secdistPath}"
      ++ lib.optional (cfg.s3Mode == "local" && cfg.seaweedfsS3ConfigPath != null) "seaweedfs_s3_config.json:${cfg.seaweedfsS3ConfigPath}"
    );
  };
}
