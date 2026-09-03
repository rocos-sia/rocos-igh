
# 插值周期配置

通过60C2/2可以配置插值周期。可通过SDO写入该对象来修改插值周期。

# PDO Configuration

## RxPDO(Outputs)

| Object(Hex) / Hi Sub(Dec) | Name | Data Type | Attribute |
| :--- | :--- | :--- | :---  |
| 607A/0 | Target Position | INT32 | RW |
| 60FE/0 | Digital outputs | UINT32 | RW |
| 6040/0 | Control word | UINT16 | RW  |


## TxPDO (Inputs)

| Object(Hex) / Hi Sub(Dec) | Name | Data Type | Attribute |
| :--- | :--- | :--- | :--- |
| 6064/0 | Position actual value | INT32 | RO  |
| 60FD/0 | Digital inputs | UINT32 | RO |
| 6041/0 | Status word | UINT16 | RO  |

