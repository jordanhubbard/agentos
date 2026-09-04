# BlockService Contract Changelog

## Version 2

- Added the required `media_id` request field to every operation.
- Defined independent Ubuntu and FreeBSD installation-media identifiers.
- Callers migrating from version 1 must set `media_id` to
  `BLK_SVC_MEDIA_UBUNTU_INSTALL` to retain the former single-device behavior.

No opcodes changed.

## Version 1

- Initial single-device block-service contract.
