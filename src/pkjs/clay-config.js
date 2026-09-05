module.exports = [
  {
    "type": "heading",
    "defaultValue": "ScaleMates Lists"
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Your Profile"
      },
      {
        "type": "input",
        "messageKey": "user_id",
        "label": "ScaleMates User ID",
        "description": "Find this in your profile URL: mate.php?id=XXXXX",
        "attributes": {
          "placeholder": "e.g. 142037",
          "type": "number",
          "limit": 10
        }
      }
    ]
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Feeds to Show"
      },
      {
        "type": "text",
        "defaultValue": "Long press Select on the watch to cycle through chosen feeds."
      },
      {
        "type": "toggle",
        "messageKey": "feed_stash",
        "label": "Stash",
        "defaultValue": true
      },
      {
        "type": "toggle",
        "messageKey": "feed_wishlist",
        "label": "Wishlist",
        "defaultValue": false
      },
      {
        "type": "toggle",
        "messageKey": "feed_started",
        "label": "Started",
        "defaultValue": false
      },
      {
        "type": "toggle",
        "messageKey": "feed_completed",
        "label": "Completed",
        "defaultValue": false
      },
      {
        "type": "toggle",
        "messageKey": "feed_forsale",
        "label": "For Sale",
        "defaultValue": false
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
];
